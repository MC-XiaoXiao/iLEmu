// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#pragma once
#include <foundation/address_space.hpp>
#include <cstddef>
#include <cstdint>
#include <span>
#include <utility>
#include <vector>

namespace ilemu {
// ARM32 sendit/sockargs copyin precedes payload copying. Protocol-specific
// ancillary validation happens later and must not change that error order.
class SocketControlBuffer {
public:
    std::uint32_t copy_from(AddressSpace& memory, std::uint32_t address,
        std::uint32_t size)
    {
        bytes_.clear();
        if (address == 0)
            return 0; // sendit ignores msg_controllen without msg_control
        // K32 sockargs permits at most one MCLBYTES cluster. This is a
        // guest kernel limit, independent of the host's pointer/mbuf sizes.
        constexpr std::uint32_t maximum_size = 2048;
        constexpr std::uint32_t header_size = 12;
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
    [[nodiscard]] bool present() const { return !bytes_.empty(); }
    [[nodiscard]] std::span<const std::byte> bytes() const { return bytes_; }
private:
    std::vector<std::byte> bytes_;
};
} // namespace ilemu
