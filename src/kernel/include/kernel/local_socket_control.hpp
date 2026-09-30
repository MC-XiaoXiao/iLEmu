// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#pragma once
#include <foundation/address_space.hpp>
#include <network/darwin_socket_abi.hpp>
#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

namespace ilemu {
// XNU sendit/sockargs copy one control mbuf before sosend copies payload.
// AF_LOCAL unp_internalize then validates that entire mbuf as SCM_RIGHTS;
// it does not walk or merge a chain of user-supplied cmsghdrs.
class LocalSocketControl {
public:
    std::uint32_t copy_from(AddressSpace& memory, std::uint32_t address,
        std::uint32_t size)
    {
        if (address == 0)
            return 0; // sendit ignores msg_controllen without msg_control
        // K32 sockargs permits at most one MCLBYTES cluster. This is a
        // guest kernel limit, independent of the host's pointer/mbuf sizes.
        constexpr std::uint32_t maximum_size = 2048;
        if (size < header_size || size > maximum_size)
            return 22; // EINVAL before copyin
        if (!memory.accessible(address, size, MemoryPermission::Read))
            return 14; // EFAULT
        auto bytes = memory.read_bytes(address, size);
        if (!bytes)
            return 14;
        bytes_ = std::move(*bytes);
        return 0;
    }
    [[nodiscard]] std::uint32_t validate() const
    {
        if (!present())
            return 0;
        if (word(0) != bytes_.size() ||
            word(4) != darwin::socket::option_level || word(8) != 1U)
            return 22;
        // Retain the existing malformed-input guard. Some public XNU
        // recvit paths panic on an unaligned control length; accepting
        // those inputs needs separate firmware evidence, not normalization.
        return (bytes_.size() - header_size) % sizeof(std::uint32_t) ? 22U : 0U;
    }
    [[nodiscard]] bool present() const { return !bytes_.empty(); }
    [[nodiscard]] std::size_t descriptor_count() const
    {
        return present() ? (bytes_.size() - header_size) / sizeof(std::uint32_t) : 0;
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
            result |= std::to_integer<std::uint32_t>(bytes_[offset + byte]) << (byte * 8U);
        return result;
    }
    std::vector<std::byte> bytes_;
};
} // namespace ilemu
