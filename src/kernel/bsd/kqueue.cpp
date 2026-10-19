// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "kernel/darwin_kqueue_abi.hpp"
#include "kernel/kernel.hpp"
#include "kernel/kevent_wire.hpp"
#include "support.hpp"
#include <algorithm>
#include <limits>
#include <mutex>
#include <string>

namespace ilemu {
namespace {
    constexpr std::uint32_t kevent_flag_workqueue = 0x20U;
}

void CompatibilityKernel::dispatch_bsd_kqueue(Cpu& cpu, std::uint32_t number)
{
    auto& registers = cpu.registers();
    switch (number) {
    case 362: { // kqueue
        const auto fd = allocate_file_descriptor();
        if (!fd) {
            bsd_error(cpu, 24); // EMFILE
            return;
        }
        virtual_descriptors_.emplace(*fd, "kqueue");
        kqueues_.emplace(*fd, std::vector<KeventRegistration> { });
        bsd_success(cpu, *fd);
        return;
    }
    case 363: // kevent
    case 369: // kevent64
    case 374: { // kevent_qos
        const bool qos = number == 374U;
        const auto format = qos ? darwin::kqueue::WireFormat::QualityOfService
                            : number == 369U
                                ? darwin::kqueue::WireFormat::Extended64
                                : darwin::kqueue::WireFormat::Legacy32;
        const KeventWireFormat wire { format };
        const auto timeout_address =
            qos ? 0U : registers[number == 369U ? 6U : 5U];
        std::uint32_t call_flags = number == 369U ? registers[5] : 0U;
        if (qos) {
            if (shared_state_->darwin_abi.kevent_abi !=
                DarwinKeventAbi::QualityOfService) {
                dispatch_bsd_nosys(
                    cpu, shared_state_->bsd_dispatch_table.sends_sigsys(number));
                return;
            }
            // ARM32 syscall wrappers marshal seven words into r0-r6.
            // XNU reads argument eight at saved SP + seven natural words.
            const auto flags = registers[13] <= UINT32_MAX - 28U
                                   ? memory_.read32(registers[13] + 28U)
                                   : std::nullopt;
            if (!flags ||
                (registers[6] != 0U && !memory_.read32(registers[6]))) {
                bsd_error(cpu, bsd_support::bad_address);
                return;
            }
            call_flags = *flags & 0x2fU;
            // Stack-oriented copyout has a different user-buffer contract.
            if ((call_flags & 0x0cU) != 0U) {
                bsd_error(cpu, 45); // ENOTSUP
                return;
            }
        }
        const auto workqueue_queue =
            qos && (call_flags & kevent_flag_workqueue) != 0U;
        const auto fd = workqueue_queue
                            ? darwin::kqueue::workqueue_descriptor
                            : registers[0];
        if (workqueue_queue)
            kqueues_.try_emplace(fd, std::vector<KeventRegistration> { });
        const auto queue = kqueues_.find(fd);
        if (queue == kqueues_.end() || registers[2] > 4096 ||
            registers[4] > 4096) {
            bsd_error(cpu, queue == kqueues_.end()
                               ? bsd_support::bad_file_descriptor
                               : bsd_support::invalid_argument);
            return;
        }
        if (workqueue_queue && registers[4] != 0U &&
            (call_flags & 0x02U) == 0U) {
            // Only the kernel's worker upcall can drain this queue. User
            // kevent_qos calls may request change-error receipts.
            bsd_error(cpu, bsd_support::invalid_argument);
            return;
        }
        const auto valid_range = [&](std::uint32_t address, std::uint32_t count,
                                     MemoryPermission permission) {
            const auto bytes = static_cast<std::uint64_t>(count) * wire.size();
            return count == 0U ||
                   (address != 0U &&
                       static_cast<std::uint64_t>(address) + bytes <=
                           (1ULL << 32U) &&
                       memory_.accessible(address,
                           static_cast<std::uint32_t>(bytes), permission));
        };
        if (!valid_range(registers[1], registers[2], MemoryPermission::Read) ||
            !valid_range(registers[3], registers[4], MemoryPermission::Write)) {
            bsd_error(cpu, bsd_support::bad_address);
            return;
        }
        if (registers[2] != 0U)
            shared_state_->note_io_event_transition();
        std::uint32_t receipts_written = 0;
        const auto write_receipt = [&](KeventValue value,
                                       std::uint32_t error) {
            if (receipts_written >= registers[4] || registers[3] == 0)
                return false;
            value.flags |= darwin::kqueue::event_error;
            value.data = error;
            if (!wire.write(memory_,
                    registers[3] + receipts_written * wire.size(), value))
                return false;
            ++receipts_written;
            return true;
        };
        for (std::uint32_t index = 0; index < registers[2]; ++index) {
            const auto value =
                wire.read(memory_, registers[1] + index * wire.size());
            if (!value) {
                bsd_error(cpu, bsd_support::bad_address);
                return;
            }
            const auto change_error = apply_kevent_change(fd, *value);
            if (change_error != 0U) {
                if ((call_flags & 2U) != 0U &&
                    write_receipt(*value, change_error))
                    continue;
                bsd_error(cpu, change_error);
                return;
            }
            if ((value->flags & darwin::kqueue::event_receipt) != 0U &&
                !write_receipt(*value, 0U)) {
                bsd_error(cpu, bsd_support::bad_address);
                return;
            }
        }
        if (workqueue_queue &&
            !request_workqueue_kevents(&cpu)) {
            bsd_error(cpu, darwin::error::no_memory);
            return;
        }
        if (receipts_written != 0 || (call_flags & 2U) != 0U) {
            bsd_success(cpu, receipts_written);
            return;
        }
        std::optional<std::uint64_t> timeout_deadline;
        bool poll_only = (call_flags & 1U) != 0U;
        if (timeout_address != 0 && !poll_only) {
            const auto seconds_word =
                memory_.read32(timeout_address +
                               darwin::kqueue::arm32_timespec::seconds_offset);
            const auto nanoseconds_word = memory_.read32(
                timeout_address +
                darwin::kqueue::arm32_timespec::nanoseconds_offset);
            if (!seconds_word || !nanoseconds_word) {
                bsd_error(cpu, bsd_support::bad_address);
                return;
            }
            const auto seconds = static_cast<std::int32_t>(*seconds_word);
            const auto nanoseconds =
                static_cast<std::int32_t>(*nanoseconds_word);
            if (seconds < 0 || nanoseconds < 0 ||
                static_cast<std::uint64_t>(nanoseconds) >=
                    darwin::kqueue::nanoseconds_per_second) {
                bsd_error(cpu, bsd_support::invalid_argument);
                return;
            }
            const auto duration = static_cast<std::uint64_t>(seconds) *
                                      darwin::kqueue::nanoseconds_per_second +
                                  static_cast<std::uint64_t>(nanoseconds);
            poll_only = duration == 0;
            const auto now = shared_state_->clock.now();
            timeout_deadline =
                duration > std::numeric_limits<std::uint64_t>::max() - now
                    ? std::numeric_limits<std::uint64_t>::max()
                    : now + duration;
        }
        if (registers[4] != 0) {
            const auto ready = collect_ready_kevents(
                cpu.processor_id(), fd, registers[3], registers[4], format);
            if (!ready) {
                bsd_error(cpu, bsd_support::bad_address);
                return;
            }
            if (*ready != 0) {
                bsd_success(cpu, *ready);
                return;
            }
        }
        if (registers[4] == 0) {
            bsd_success(cpu, 0);
            return;
        }
        if (poll_only) {
            bsd_success(cpu, 0);
            return;
        }
        process_.waiting_for_events = true;
        pending_kevents_[cpu.processor_id()] = PendingKevent { fd, registers[3],
            registers[4], cpu.processor_id(), timeout_deadline, format };
        output_.write(
            "[network] kevent wait pid=" + std::to_string(process_.pid) +
            " fd=" + std::to_string(fd) +
            " registrations=" + std::to_string(queue->second.size()) + "\n");
        bsd_success(cpu, 0);
        cpu.halt(Dynarmic::HaltReason::UserDefined5);
        return;
    }
    }
}
} // namespace ilemu
