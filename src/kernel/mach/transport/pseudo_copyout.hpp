// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
#pragma once
#include "port_copyout.hpp"
#include "foundation/address_space.hpp"
#include "kernel/darwin_abi.hpp"
#include "kernel/mach_descriptor_transport.hpp"
#include "mach/mig_wire_abi.hpp"
#include <algorithm>

namespace ilemu::mach_transport {

// ipc_kmsg_copyout_pseudo returns a failed send to its sender. Header ports
// are treated as body rights, without swapping; errors do not stop copyout.
// Hold mach_mutex and transfer CopyinCleanup ownership before calling restore.
class PseudoCopyout {
public:
    PseudoCopyout(KernelSharedState& state, AddressSpace& memory, std::uint32_t task)
        : state_(state), memory_(memory), task_(task), copyout_(state, task) { }

    // ipc_kmsg_put may copy a writable prefix before a user-page fault. Its
    // failure must not replace the send error or undo installed rights.
    static void write_back(AddressSpace& memory, std::uint32_t address,
        std::span<const std::byte> bytes)
    {
        while (!bytes.empty()) {
            const auto count = std::min<std::size_t>(bytes.size(),
                AddressSpace::page_size - (address & (AddressSpace::page_size - 1U)));
            if (!memory.accessible(address, count, MemoryPermission::Write) ||
                !memory.copy_in(address, bytes.first(count)))
                return;
            address += static_cast<std::uint32_t>(count);
            bytes = bytes.subspan(count);
        }
    }

    [[nodiscard]] std::uint32_t restore(KernelSharedState::MachMessage& message,
        std::vector<std::byte>& bytes, std::span<const Descriptor> descriptors,
        std::uint32_t destination, xnu::ipc::Right destination_right)
    {
        using namespace mach_support;
        const auto bits = read_little_word(bytes, 0);
        write_little_word(bytes, 0,
            (bits & 0xff000000U & ~darwin::mach_message::bits_circular) |
            darwin::mig_wire::received_port_disposition(bits & 0xffU) |
            (darwin::mig_wire::received_port_disposition((bits >> 8U) & 0xffU) << 8U) |
            (darwin::mig_wire::received_port_disposition((bits >> 16U) & 0xffU) << 16U));
        write_little_word(bytes, darwin::mig_wire::header_size_offset,
            static_cast<std::uint32_t>(bytes.size()));
        write_little_word(bytes, darwin::mig_wire::header_remote_port_offset,
            right(destination, destination_right, message.destination_send_object.has_value()));
        message.destination_send_object.reset();
        message.destination_send_once_object.reset();
        if (message.reply_object && message.reply_right) {
            write_little_word(bytes, darwin::mig_wire::header_local_port_offset,
                right(*message.reply_object, *message.reply_right));
            message.reply_object.reset();
        }
        if (message.voucher_object && message.voucher_right) {
            write_little_word(bytes, darwin::mig_wire::header_voucher_offset,
                right(*message.voucher_object, *message.voucher_right));
            message.voucher_object.reset();
        }
        // Older compact copyin walks backwards; pseudo-copyout always walks
        // the wire layout forwards, including elements of port arrays.
        std::sort(message.port_transfers.begin(), message.port_transfers.end(),
            [](const auto& a, const auto& b) {
                return a.descriptor_offset != b.descriptor_offset
                    ? a.descriptor_offset < b.descriptor_offset : a.array_index < b.array_index;
            });
        std::sort(message.ool_port_arrays.begin(), message.ool_port_arrays.end(),
            [](const auto& a, const auto& b) { return a.descriptor_offset < b.descriptor_offset; });
        std::size_t transfer_index = 0, payload_index = 0, array_index = 0;
        for (const auto& descriptor : descriptors) {
            auto metadata = read_little_word(bytes, descriptor.offset + 8U);
            if (descriptor.kind == DescriptorKind::Port) {
                if (transfer_index < message.port_transfers.size() &&
                    message.port_transfers[transfer_index].descriptor_offset == descriptor.offset) {
                    const auto& transfer = message.port_transfers[transfer_index++];
                    write_little_word(bytes, descriptor.offset, right(transfer.object, transfer.right));
                }
                metadata = darwin::mig_wire::replace_descriptor_disposition(metadata,
                    darwin::mig_wire::received_port_disposition(descriptor.disposition()));
            } else if (descriptor.kind == DescriptorKind::OutOfLineMemory) {
                const auto& payload = message.ool_payloads[payload_index++];
                const auto address = allocate(static_cast<std::uint32_t>(payload.bytes.size()));
                if (address && !memory_.copy_in(*address, payload.bytes))
                    errors_ |= darwin::mach_message::vm_space;
                write_little_word(bytes, descriptor.offset, address.value_or(0));
                if (!address)
                    write_little_word(bytes, descriptor.offset + 4U, 0);
                const auto copy = (metadata >> 8U) & 0xffU;
                metadata = (metadata & ~0xffU) | (copy == darwin::mig_wire::ool_copy_virtual ? 1U : 0U);
            } else if (descriptor.kind == DescriptorKind::OutOfLinePorts) {
                const auto& array = message.ool_port_arrays[array_index++];
                const auto size = array.count * darwin::mig_wire::word_size;
                // Native copyout allocates the array before installing names.
                const auto address = allocate(size);
                std::vector<std::byte> names(address ? size : 0);
                if (address) {
                    for (const auto element : array.dead_elements)
                        write_little_word(names, element * darwin::mig_wire::word_size, xnu::ipc::dead_name);
                }
                while (transfer_index < message.port_transfers.size() &&
                    message.port_transfers[transfer_index].descriptor_offset == descriptor.offset) {
                    const auto& transfer = message.port_transfers[transfer_index++];
                    if (address) {
                        write_little_word(names, *transfer.array_index * darwin::mig_wire::word_size,
                            right(transfer.object, transfer.right));
                    } else {
                        discard(transfer.object, transfer.right);
                    }
                }
                if (address && !memory_.copy_in(*address, names))
                    errors_ |= darwin::mach_message::vm_space;
                write_little_word(bytes, descriptor.offset, address.value_or(0));
                metadata = darwin::mig_wire::replace_descriptor_disposition(metadata,
                    darwin::mig_wire::received_port_disposition(descriptor.disposition()));
                metadata = (metadata & ~0xffffU) | (darwin::mig_wire::ool_copy_virtual << 8U) | 1U;
            }
            write_little_word(bytes, descriptor.offset + 8U, metadata);
        }
        message.port_transfers.clear();
        return errors_;
    }

private:
    [[nodiscard]] std::uint32_t right(std::uint32_t object,
        xnu::ipc::Right type, bool held_send = true)
    {
        const auto name = copyout_(object, type);
        if (!name) {
            errors_ |= darwin::mach_message::ipc_space;
            if (type != xnu::ipc::Right::Send || held_send)
                discard(object, type);
            return xnu::ipc::null_name;
        }
        if (type == xnu::ipc::Right::Send && held_send)
            mach_support::release_inflight_send_right_locked(state_, object);
        if (type == xnu::ipc::Right::Receive) {
            state_.note_mach_queue_topology_change_locked(object);
            static_cast<void>(state_.remove_mach_port_set_member_from_all_locked(object));
            static_cast<void>(state_.mach_port_objects.set_receive_owner(object, task_));
        }
        return *name;
    }
    void discard(std::uint32_t object, xnu::ipc::Right type)
    {
        KernelSharedState::MachMessage failed;
        failed.reply_object = object;
        failed.reply_right = type;
        mach_support::discard_mach_message_rights_locked(state_, failed);
    }
    [[nodiscard]] std::optional<std::uint32_t> allocate(std::uint32_t size)
    {
        if (size == 0)
            return 0U;
        const auto mapped_size = (size + AddressSpace::page_size - 1U) &
            ~(AddressSpace::page_size - 1U);
        const auto address = mach_support::find_free_guest_region(
            memory_, mach_support::ool_receive_base, mapped_size);
        if (!address || !memory_.map(*address, mapped_size,
                MemoryPermission::Read | MemoryPermission::Write)) {
            errors_ |= darwin::mach_message::vm_space;
            return std::nullopt;
        }
        return address;
    }
    KernelSharedState& state_;
    AddressSpace& memory_;
    std::uint32_t task_;
    PortCopyout copyout_;
    std::uint32_t errors_ { };
};

} // namespace ilemu::mach_transport
