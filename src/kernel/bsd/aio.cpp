// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// Dispatch guest asynchronous file I/O requests and completion
// queries.
//
// Apple public ABI/behavior references (guest profiles may differ):
// https://github.com/apple-oss-distributions/xnu/blob/xnu-792.24.17/bsd/kern/kern_aio.c

#include "kernel/kernel.hpp"

#include "kernel/darwin_abi.hpp"

#include <algorithm>
#include <bit>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include <sys/stat.h>
#include <unistd.h>

#include "support.hpp"

namespace ilemu {
namespace {

    enum class AioOperation {
        Synchronize,
        Read,
        Write,
    };

    struct GuestAioControlBlock {
        std::uint32_t descriptor { };
        std::int64_t offset { };
        std::uint32_t buffer { };
        std::uint32_t byte_count { };
        std::uint32_t notification { };
        std::uint32_t signal { };
    };

    std::optional<GuestAioControlBlock> read_aio_control_block(
        const AddressSpace& memory, std::uint32_t address)
    {
        using namespace darwin::aio;
        const auto bytes = memory.read_bytes(address, control_block_size);
        if (!bytes) {
            return std::nullopt;
        }
        const auto read32 = [&](std::size_t offset) {
            std::uint32_t value { };
            for (std::size_t index = 0; index < sizeof(value); ++index) {
                value |=
                    static_cast<std::uint32_t>(
                        std::to_integer<std::uint8_t>((*bytes)[offset + index]))
                    << (index * 8U);
            }
            return value;
        };
        const auto read64 = [&](std::size_t offset) {
            std::uint64_t value { };
            for (std::size_t index = 0; index < sizeof(value); ++index) {
                value |=
                    static_cast<std::uint64_t>(
                        std::to_integer<std::uint8_t>((*bytes)[offset + index]))
                    << (index * 8U);
            }
            return value;
        };
        return GuestAioControlBlock { read32(descriptor_offset),
            std::bit_cast<std::int64_t>(read64(file_offset_offset)),
            read32(buffer_offset), read32(byte_count_offset),
            read32(notification_offset), read32(signal_offset) };
    }

} // namespace

void CompatibilityKernel::dispatch_bsd_aio(Cpu& cpu, std::uint32_t number)
{
    const TaskVmEvents::Scope user_access { memory_.task_vm_events() };
    service_file_io();
    service_completed_file_renames();
    auto& registers = cpu.registers();
    const auto submit = [&](std::uint32_t address, AioOperation kind) {
        if (address == 0 || aio_completions_.contains(address) ||
            aio_completions_.size() >= darwin::aio::maximum_requests_per_process) {
            bsd_error(cpu, darwin::error::would_block);
            return;
        }
        const auto block = read_aio_control_block(memory_, address);
        if (!block) {
            bsd_error(cpu, darwin::error::would_block);
            return;
        }
        if (kind != AioOperation::Synchronize &&
            (block->offset < 0 || block->buffer == 0 ||
                block->byte_count > bsd_support::maximum_io)) {
            bsd_error(cpu, darwin::error::invalid_argument);
            return;
        }
        if (block->notification != darwin::aio::notify_none &&
            (block->notification != darwin::aio::notify_signal ||
                block->signal == 0 || block->signal >= darwin::signal::count ||
                block->signal == darwin::signal::kill || block->signal == darwin::signal::stop)) {
            bsd_error(cpu, darwin::error::invalid_argument);
            return;
        }
        auto fd = block->descriptor;
        if (const auto duplicate = duplicated_descriptors_.find(fd);
            duplicate != duplicated_descriptors_.end())
            fd = duplicate->second;
        if (!file_descriptors_.contains(fd)) {
            bsd_error(cpu, descriptor_valid(fd) ? darwin::error::illegal_seek
                                               : darwin::error::bad_file_descriptor);
            return;
        }
        if (shared_state_->filesystem_renames_pending.load(std::memory_order_acquire) != 0 &&
            !regular_file_open_descriptions_.contains(fd)) {
            static_cast<void>(defer_filesystem_dispatch(cpu, number));
            return;
        }
        const auto operation_kind = kind == AioOperation::Read ? FileIoOperation::Kind::Read
            : kind == AioOperation::Write ? FileIoOperation::Kind::Write
                                         : FileIoOperation::Kind::Synchronize;
        auto operation = prepare_file_io(cpu, fd, operation_kind, block->buffer,
            kind == AioOperation::Synchronize ? 0 : block->byte_count,
            kind == AioOperation::Synchronize ? 0 : static_cast<std::uint64_t>(block->offset), {}, true);
        if (!operation)
            return;
        if (block->notification == darwin::aio::notify_signal)
            operation->notification_signal = block->signal;
        aio_completions_.emplace(address, AioCompletion { block->descriptor, std::move(operation) });
        bsd_success(cpu, 0);
    };
    switch (number) {
    case darwin::syscall::aio_synchronize:
        if (registers[0] != 0 && registers[0] != darwin::aio::synchronize)
            bsd_error(cpu, darwin::error::invalid_argument);
        else
            submit(registers[1], AioOperation::Synchronize);
        return;
    case darwin::syscall::aio_read:
        submit(registers[0], AioOperation::Read);
        return;
    case darwin::syscall::aio_write:
        submit(registers[0], AioOperation::Write);
        return;
    case darwin::syscall::aio_error:
    case darwin::syscall::aio_return: {
        const auto found = aio_completions_.find(registers[0]);
        if (found == aio_completions_.end()) {
            bsd_error(cpu, darwin::error::invalid_argument);
            return;
        }
        const auto& operation = found->second.operation;
        if (number == darwin::syscall::aio_error) {
            bsd_success(cpu, operation->committed ? operation->error
                                                : darwin::error::operation_in_progress);
        } else if (!operation->committed) {
            bsd_error(cpu, darwin::error::invalid_argument);
        } else {
            const auto result = operation->result;
            aio_completions_.erase(found);
            bsd_success(cpu, static_cast<std::uint32_t>(result));
        }
        return;
    }
    case darwin::syscall::aio_cancel: {
        const auto fd = registers[0];
        const auto address = registers[1];
        if (!descriptor_valid(fd)) {
            bsd_error(cpu, darwin::error::bad_file_descriptor);
            return;
        }
        auto resolved = fd;
        if (const auto duplicate = duplicated_descriptors_.find(resolved);
            duplicate != duplicated_descriptors_.end())
            resolved = duplicate->second;
        const auto description = ensure_regular_file_open_description(resolved);
        if (!description) {
            bsd_error(cpu, darwin::error::bad_file_descriptor);
            return;
        }
        bool matched = false, cancelled = false, running = false;
        std::vector<std::uint32_t> notifications;
        for (const auto& [candidate, completion] : aio_completions_) {
            if (completion.descriptor != fd || (address && candidate != address) ||
                completion.operation->description_identifier != description->identifier())
                continue;
            matched = true;
            const auto& operation = completion.operation;
            if (operation->committed)
                continue;
            if (operation->request || operation->sync_request) {
                running = true;
            } else {
                shared_state_->file_io.cancel(operation);
                if (operation->notification_signal)
                    notifications.push_back(operation->notification_signal);
                cancelled = true;
            }
        }
        if (address && !matched)
            bsd_error(cpu, darwin::error::invalid_argument);
        else
            bsd_success(cpu, running ? darwin::aio::not_canceled
                : cancelled ? darwin::aio::canceled : darwin::aio::all_done);
        shared_state_->note_io_event_transition();
        for (const auto signal : notifications)
            static_cast<void>(deliver_signal(signal));
        if (process_.exited)
            cpu.halt(Dynarmic::HaltReason::UserDefined1);
        return;
    }
    case darwin::syscall::aio_suspend: {
        const auto count = static_cast<std::int32_t>(registers[1]);
        if (count < 1 || count > static_cast<std::int32_t>(darwin::aio::maximum_requests_per_process) ||
            registers[0] == 0) {
            bsd_error(cpu, darwin::error::invalid_argument);
            return;
        }
        PendingAioSuspend pending;
        for (std::int32_t i = 0; i < count; ++i) {
            const auto address = memory_.read32(registers[0] + static_cast<std::uint32_t>(i) * 4U);
            if (!address) {
                bsd_error(cpu, darwin::error::would_block);
                return;
            }
            if (const auto found = aio_completions_.find(*address); found != aio_completions_.end())
                pending.operations.push_back(found->second.operation);
        }
        if (pending.operations.empty()) {
            bsd_error(cpu, darwin::error::invalid_argument);
            return;
        }
        if (registers[2]) {
            const auto seconds = memory_.read32(registers[2]);
            const auto nanos = memory_.read32(registers[2] + 4U);
            if (!seconds || !nanos) {
                bsd_error(cpu, darwin::error::bad_address);
                return;
            }
            if (*seconds > INT32_MAX || *nanos >= VirtualClock::nanoseconds_per_second) {
                bsd_error(cpu, darwin::error::invalid_argument);
                return;
            }
            pending.deadline = shared_state_->clock.now() +
                static_cast<std::uint64_t>(*seconds) * VirtualClock::nanoseconds_per_second + *nanos;
        }
        pending_aio_suspends_.insert_or_assign(cpu.processor_id(), std::move(pending));
        if (!deliver_pending_aio_suspend(cpu)) {
            record_bsd_sleep();
            process_.waiting_for_events = true;
            cpu.halt(Dynarmic::HaltReason::UserDefined5);
        }
        return;
    }
    default:
        trace_unknown(cpu, "BSD AIO syscall", number);
        bsd_error(cpu, bsd_support::not_implemented);
        return;
    }
}

bool CompatibilityKernel::deliver_pending_aio_suspend(Cpu& cpu)
{
    service_file_io();
    const auto found = pending_aio_suspends_.find(cpu.processor_id());
    if (found == pending_aio_suspends_.end())
        return false;
    const auto& pending = found->second;
    const auto ready = std::any_of(pending.operations.begin(), pending.operations.end(),
        [](const auto& operation) { return operation->committed; });
    if (!ready && (!pending.deadline || shared_state_->clock.now() < *pending.deadline))
        return false;
    pending_aio_suspends_.erase(found);
    if (ready)
        bsd_success(cpu, 0);
    else
        bsd_error(cpu, darwin::error::would_block);
    process_.waiting_for_events = false;
    cpu.clear_halt();
    return true;
}
} // namespace ilemu
