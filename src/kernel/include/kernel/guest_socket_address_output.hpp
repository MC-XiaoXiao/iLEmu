// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
#pragma once
#include <kernel/guest_read_buffer.hpp>
#include <optional>

namespace ilemu {
// Capture the user capacity before protocol operations or a blocking wait.
// Payload and address copyout may alias the original length word.
class GuestSocketAddressOutput {
public:
    GuestSocketAddressOutput() = default;
    static std::optional<GuestSocketAddressOutput> capture(AddressSpace& memory,
        std::uint32_t address, std::uint32_t length_address)
    {
        const TaskVmEvents::Scope user_access { memory.task_vm_events() };
        const auto capacity = memory.read32(length_address);
        if (!capacity) return std::nullopt;
        return GuestSocketAddressOutput {address, length_address, *capacity};
    }

    bool copy(AddressSpace& memory, std::span<const std::byte> bytes) const
    {
        const TaskVmEvents::Scope user_access { memory.task_vm_events() };
        const auto count = std::min<std::size_t>(capacity_, bytes.size());
        return (count == 0 || GuestReadBuffer {address_}.copy(memory, bytes.first(count))) &&
               memory.write32(length_address_, static_cast<std::uint32_t>(bytes.size()));
    }

    bool copy_optional(AddressSpace& memory, std::span<const std::byte> bytes) const
    {
        return address_ == 0 || copy(memory, bytes);
    }

    static bool copy_received_name(AddressSpace& memory, std::uint32_t address,
        std::uint32_t& length, std::span<const std::byte> bytes)
    {
        if (address == 0) return true;
        // ARM32 recvit/copyout_sa uses a signed native length. A missing
        // source or nonpositive capacity reports zero (1A420 and XNU 1228+).
        if (length == 0 || length > 0x7fffffffU) bytes = {};
        const auto count = std::min<std::size_t>(length, bytes.size());
        if (count != 0 && !GuestReadBuffer {address}.copy(memory, bytes.first(count)))
            return false;
        length = static_cast<std::uint32_t>(bytes.size());
        return true;
    }

    bool copy_received(AddressSpace& memory, std::span<const std::byte> bytes) const
    {
        if (address_ == 0 || length_address_ == 0) return true;
        const TaskVmEvents::Scope user_access { memory.task_vm_events() };
        auto length = capacity_;
        return copy_received_name(memory, address_, length, bytes) &&
               memory.write32(length_address_, length);
    }

private:
    GuestSocketAddressOutput(std::uint32_t address,
        std::uint32_t length_address, std::uint32_t capacity)
        : address_(address), length_address_(length_address), capacity_(capacity) {}
    std::uint32_t address_ {};
    std::uint32_t length_address_ {};
    std::uint32_t capacity_ {};
};
} // namespace ilemu
