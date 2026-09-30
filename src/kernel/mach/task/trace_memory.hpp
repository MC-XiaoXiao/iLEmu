// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#pragma once

#include "../transport/kernel_reply.hpp"
#include "kernel/mach_task_trace_abi.hpp"
#include "kernel/mach_task_info_abi.hpp"
#include "mach/task_mig_ids.hpp"

#include <limits>
#include <new>

namespace ilemu::task_mig {
// Callers hold mach_mutex, serializing registration with process teardown.
class TraceMemory {
public:
    static bool handles(std::uint32_t identifier)
    {
        return identifier == xnu::mig::task::id(
            xnu::mig::task::Routine::task_set_info);
    }

    static std::optional<std::uint32_t> dispatch_locked(AddressSpace& memory,
        KernelSharedState& state, std::uint32_t caller, std::uint32_t object,
        KernelSharedState::MachMessage& request)
    {
        using namespace mach_support;
        const auto identifier = read_little_word(request.bytes, 20U);
        auto error = darwin::mig::bad_arguments;
        if (request.bytes.size() >= 40U &&
            !(read_little_word(request.bytes, 0U) &
                darwin::mig_wire::message_complex_bit)) {
            const auto count = read_little_word(request.bytes, 36U);
            if (count <= darwin::mach::task_info::maximum_input_words(
                             state.darwin_abi.abi_epoch) &&
                request.bytes.size() == 40U + count * 4U) {
                error = darwin::mach::invalid_argument;
                const auto task = state.task_port_pids.find(object);
                if (task != state.task_port_pids.end() && task->second == caller &&
                    state.darwin_abi.task_trace_memory !=
                        DarwinTaskTraceMemoryAbi::Unsupported &&
                    read_little_word(request.bytes, 32U) ==
                        darwin::task_trace::information_flavor &&
                    count == darwin::task_trace::information_count) {
                    error = register_locked(memory, state, caller,
                        read_wide(request.bytes, 40U),
                        read_wide(request.bytes, 48U),
                        read_wide(request.bytes, 56U));
                }
            }
        }
        return mach_ipc::enqueue_kernel_reply_locked(state, request,
            identifier, std::array { 0U, 1U, error });
    }

    static bool inspect_locked(KernelSharedState& state, std::uint32_t pid,
        std::uint64_t incarnation)
    {
        using namespace mach_support;
        using namespace darwin::task_trace;
        const auto registration = state.task_trace_memory.find(pid);
        const auto destination = state.host_special_ports.find(notification_port);
        if (registration == state.task_trace_memory.end() ||
            destination == state.host_special_ports.end() ||
            !state.mach_port_objects.contains(destination->second) ||
            state.mach_ports_being_removed.contains(destination->second))
            return false;
        const auto endpoint = destination->second;
        const auto memory = registration->second.memory_object;
        KernelSharedState::MachMessage message;
        message.bytes.resize(inspect_message_size);
        write_little_word(message.bytes, 0U, 0x80000011U); // MOVE_SEND
        write_little_word(message.bytes, 4U, inspect_message_size);
        write_little_word(message.bytes, 8U, endpoint);
        write_little_word(message.bytes, 20U, inspect_message);
        write_little_word(message.bytes, 24U, 1U);
        write_little_word(message.bytes, 28U, memory);
        write_little_word(message.bytes, 36U, 19U << 16U); // COPY_SEND
        write_little_word(message.bytes, 44U, 1U); // NDR: little endian
        write_little_word(message.bytes, 48U, pid);
        write_wide(message.bytes, 52U, incarnation);
        write_wide(message.bytes, 60U, registration->second.buffer_size);
        message.destination = endpoint;
        message.destination_send_object = endpoint;
        message.port_transfers.push_back({ 28U, memory, std::nullopt,
            memory, xnu::ipc::Right::Send, 19U });
        // The queued message owns rights independently of the task and host
        // slots. Ordinary receive/discard releases these in-flight references.
        ++state.mach_inflight_send_rights[endpoint];
        ++state.mach_inflight_send_rights[memory];
        state.enqueue_mach_message_locked(endpoint, std::move(message));
        return true;
    }

private:
    static std::uint64_t read_wide(
        const std::vector<std::byte>& bytes, std::size_t offset)
    {
        return mach_support::read_little_word(bytes, offset) |
            (std::uint64_t { mach_support::read_little_word(bytes, offset + 4U) }
                << 32U);
    }

    static void write_wide(std::vector<std::byte>& bytes, std::size_t offset,
        std::uint64_t value)
    {
        mach_support::write_little_word(bytes, offset,
            static_cast<std::uint32_t>(value));
        mach_support::write_little_word(bytes, offset + 4U,
            static_cast<std::uint32_t>(value >> 32U));
    }

    static std::uint32_t register_locked(AddressSpace& memory,
        KernelSharedState& state, std::uint32_t pid, std::uint64_t address,
        std::uint64_t size, std::uint64_t mailbox)
    {
        using namespace darwin::task_trace;
        constexpr auto mask = AddressSpace::page_size - 1U;
        if (state.task_trace_memory.contains(pid) || address == 0 ||
            size == 0 || (size & mask) || size > maximum_buffer_size ||
            mailbox == 0 || mailbox >= size ||
            mailbox > maximum_mailbox_size || (mailbox & 4095U))
            return darwin::mach::invalid_argument;
        if (address > std::numeric_limits<std::uint32_t>::max())
            return darwin::mach::invalid_address;
        // Without MAP_MEM_USE_DATA_ADDR, vm_user.c truncates the address
        // and rounds the size independently.
        const auto source = static_cast<std::uint32_t>(address) & ~mask;
        const auto length = static_cast<std::uint32_t>(size);
        if (!memory.accessible(source, length, MemoryPermission::None))
            return darwin::mach::invalid_address;
        if (!memory.accessible(source, length, MemoryPermission::Read))
            return 2U; // KERN_PROTECTION_FAILURE
        std::uint32_t object = 0;
        try {
            auto pages = memory.share_pages(source, length);
            if (!pages)
                return darwin::mach::invalid_address;
            KernelSharedState::MachMemoryEntry entry;
            entry.object = std::make_shared<KernelSharedState::MachMemoryObject>();
            entry.object->pages = std::move(*pages);
            entry.size = size;
            entry.protection = 1U; // VM_PROT_READ
            object = state.allocate_mach_object();
            if (!state.mach_port_objects.create(object))
                return darwin::mach::resource_shortage;
            state.mach_queues.try_emplace(object);
            state.mach_memory_entries.emplace(object, std::move(entry));
            state.task_trace_memory.emplace(pid,
                KernelSharedState::TaskTraceMemory { object, size, mailbox });
            mach_support::retain_kernel_send_right_locked(state, object);
        } catch (const std::bad_alloc&) {
            state.task_trace_memory.erase(pid);
            if (object != 0)
                mach_support::remove_port_object_locked(state, object);
            return darwin::mach::no_space;
        }
        return darwin::mach::success;
    }
};
} // namespace ilemu::task_mig
