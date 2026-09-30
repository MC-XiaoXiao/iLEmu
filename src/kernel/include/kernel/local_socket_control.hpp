// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#pragma once
#include <kernel/socket_control_buffer.hpp>
#include <network/darwin_socket_abi.hpp>
#include <cstddef>
#include <cstdint>

namespace ilemu {
// XNU sendit/sockargs copy one control mbuf before sosend copies payload.
// AF_LOCAL unp_internalize then validates that entire mbuf as SCM_RIGHTS;
// it does not walk or merge a chain of user-supplied cmsghdrs.
class LocalSocketControl {
public:
    std::uint32_t copy_from(AddressSpace& memory, std::uint32_t address,
        std::uint32_t size)
    {
        return buffer_.copy_from(memory, address, size);
    }
    [[nodiscard]] std::uint32_t validate() const
    {
        if (!present())
            return 0;
        if (word(0) != buffer_.bytes().size() ||
            word(4) != darwin::socket::option_level || word(8) != 1U)
            return 22;
        // Retain the existing malformed-input guard. Some public XNU
        // recvit paths panic on an unaligned control length; accepting
        // those inputs needs separate firmware evidence, not normalization.
        return (buffer_.bytes().size() - header_size) % sizeof(std::uint32_t) ? 22U : 0U;
    }
    [[nodiscard]] bool present() const { return buffer_.present(); }
    [[nodiscard]] std::size_t descriptor_count() const
    {
        return present() ? (buffer_.bytes().size() - header_size) / sizeof(std::uint32_t) : 0;
    }
    [[nodiscard]] std::uint32_t descriptor(std::size_t index) const
    {
        return word(header_size + index * sizeof(std::uint32_t));
    }
private:
    static constexpr std::uint32_t header_size = 12;
    [[nodiscard]] std::uint32_t word(std::size_t offset) const
    {
        std::uint32_t result = 0;
        for (unsigned byte = 0; byte < sizeof(result); ++byte)
            result |= std::to_integer<std::uint32_t>(buffer_.bytes()[offset + byte]) << (byte * 8U);
        return result;
    }
    SocketControlBuffer buffer_;
};
} // namespace ilemu
