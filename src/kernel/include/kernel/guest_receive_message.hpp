// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
#pragma once
#include <kernel/guest_read_buffer.hpp>
#include <kernel/guest_socket_address_output.hpp>
#include <array>

namespace ilemu {
// XNU recvmsg imports msghdr/iovecs once, before recvit can sleep. Move
// the already allocated vectors into the pending call without re-importing.
class GuestReceiveMessage {
    using Header = std::array<std::uint32_t, darwin::socket::arm32_message::size / 4>;
public:
    std::uint32_t import(AddressSpace& memory, std::uint32_t address,
        std::uint32_t flags, DarwinAbiEpoch epoch, std::size_t maximum_io)
    {
        using namespace darwin::socket;
        const TaskVmEvents::Scope user_access { memory.task_vm_events() };
        address_ = address;
        if (!memory.accessible(address, arm32_message::size, MemoryPermission::Read))
            return 14; // EFAULT; write access is checked only at final copyout
        for (std::uint32_t i = 0; i < header_.size(); ++i) {
            const auto word = memory.read32(address + i * 4U);
            if (!word) return 14;
            header_[i] = *word;
        }
        const auto count = field(arm32_message::iov_count_offset);
        if (count == 0 || count > darwin::io::maximum_vector_count)
            return 40; // EMSGSIZE, unlike readv
        field(arm32_message::flags_offset) = flags;
        if (const auto error = GuestReadBuffer::import(memory,
                field(arm32_message::iov_offset), count, epoch, vectors_, capacity_))
            return error;
        // Retain the existing receive transport bound. The imported iovec
        // lengths and pointers themselves are never rewritten in guest memory.
        if (capacity_ > maximum_io) return 22;
        return 0;
    }

    [[nodiscard]] const std::vector<GuestReadVector>& vectors() const { return vectors_; }
    [[nodiscard]] std::size_t capacity() const { return static_cast<std::size_t>(capacity_); }
    [[nodiscard]] std::uint32_t flags() const
    {
        return field(darwin::socket::arm32_message::flags_offset);
    }
    [[nodiscard]] std::uint32_t control_address() const
    {
        return field(darwin::socket::arm32_message::control_offset);
    }
    [[nodiscard]] std::uint32_t control_capacity() const
    {
        return field(darwin::socket::arm32_message::control_length_offset);
    }
    bool copy_name(AddressSpace& memory, std::span<const std::byte> bytes)
    {
        using namespace darwin::socket;
        return GuestSocketAddressOutput::copy_received_name(memory,
            field(arm32_message::name_offset),
            field(arm32_message::name_length_offset), bytes);
    }
    bool finish(AddressSpace& memory, std::uint32_t control_length,
        std::uint32_t result_flags)
    {
        using namespace darwin::socket;
        if (control_address() != 0)
            field(arm32_message::control_length_offset) = control_length;
        field(arm32_message::flags_offset) |= result_flags;
        std::array<std::byte, arm32_message::size> bytes {};
        for (std::size_t i = 0; i < header_.size(); ++i)
            for (std::size_t b = 0; b < 4; ++b)
                bytes[i * 4 + b] = static_cast<std::byte>(header_[i] >> (b * 8));
        // Preserve native full-header copyout, including prefix faults and
        // original pointers when payload/another thread overwrote the header.
        return GuestReadBuffer {address_}.copy(memory, bytes);
    }

private:
    std::uint32_t& field(std::size_t offset) { return header_[offset / 4]; }
    std::uint32_t field(std::size_t offset) const { return header_[offset / 4]; }
    std::uint32_t address_ {};
    Header header_ {};
    std::vector<GuestReadVector> vectors_;
    std::uint64_t capacity_ {};
};
} // namespace ilemu
