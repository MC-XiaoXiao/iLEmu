// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "foundation/address_space.hpp"
#include "kernel/kernel_iokit_hid.hpp"
#include "kernel/kernel_shared_state.hpp"
#include "mach/mig_wire_abi.hpp"

namespace ilemu::kernel_iokit::hid {
namespace {
    // IODataQueue's public shared-memory ABI: size, head, tail, then entries
    // containing a byte count and the firmware-serialized event data.
    bool enqueue(AddressSpace& memory, std::uint32_t queue,
        std::span<const std::byte> data)
    {
        const auto capacity = memory.read32(queue).value();
        const auto head = memory.read32(queue + 4U).value();
        const auto tail = memory.read32(queue + 8U).value();
        const auto length = static_cast<std::uint32_t>(data.size());
        const auto entry_size = length + 4U;
        auto offset = tail;
        if (tail >= head) {
            if (tail + entry_size > capacity) {
                if (head <= entry_size)
                    return false;
                if (capacity - tail >= 4U)
                    static_cast<void>(
                        memory.write32(queue + 12U + tail, length));
                offset = 0U;
            }
        } else if (head - tail <= entry_size) {
            return false;
        }
        static_cast<void>(memory.write32(queue + 12U + offset, length));
        static_cast<void>(memory.copy_in(queue + 16U + offset, data));
        static_cast<void>(memory.write32(queue + 8U, offset + entry_size));
        return head == tail || memory.read32(queue + 4U).value() == tail;
    }

    KernelSharedState::MachMessage notification(std::uint32_t port)
    {
        using namespace darwin::mig_wire;
        KernelSharedState::MachMessage message;
        message.bytes.resize(message_header_size);
        const auto word = [&message](std::size_t offset, std::uint32_t value) {
            for (std::size_t i = 0; i < sizeof(value); ++i)
                message.bytes[offset + i] =
                    static_cast<std::byte>(value >> (i * 8U));
        };
        word(header_bits_offset, message_bits(disposition_copy_send));
        word(header_size_offset, message_header_size);
        word(header_remote_port_offset, port);
        message.destination = port;
        return message;
    }
}

void enqueue_event_report(AddressSpace& memory, KernelSharedState& state,
    std::uint32_t process, std::uint32_t service,
    std::span<const std::byte> data)
{
    std::lock_guard lock { state.mach_mutex };
    for (const auto& [object, connection] : state.iokit_connections) {
        if (connection.service_port != service ||
            connection.owner_pid != process)
            continue;
        const auto client = state.iokit_hid_connections.find(object);
        if (client == state.iokit_hid_connections.end() ||
            !client->second.started)
            continue;
        const auto mapping = client->second.memory_mappings.find(0U);
        if (mapping == client->second.memory_mappings.end())
            continue;
        if (enqueue(memory, mapping->second.address, data))
            state.enqueue_mach_message_locked(client->second.notification_port,
                notification(client->second.notification_port));
    }
}

} // namespace ilemu::kernel_iokit::hid
