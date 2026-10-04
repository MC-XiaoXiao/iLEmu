// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
#pragma once

// XNU bsd/sys/kauth.h: a kauth_filesec header precedes up to 128 ACEs.

#include "foundation/address_space.hpp"
#include "kernel/darwin_abi.hpp"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <utility>
#include <vector>

namespace ilemu::bsd::file_security {

inline constexpr char attribute_name[] = "com.apple.system.Security";

struct GuestFileSecurity {
    std::vector<std::byte> bytes;
    std::uint32_t error { };

    [[nodiscard]] static GuestFileSecurity read(
        const AddressSpace& memory, std::uint32_t address)
    {
        constexpr std::uint32_t magic = 0x012cc16dU;
        constexpr std::uint32_t no_acl = std::numeric_limits<std::uint32_t>::max();
        constexpr std::uint32_t maximum_entries = 128U;
        constexpr std::size_t header_size = 44U;
        constexpr std::size_t entry_size = 24U;
        if (address > std::numeric_limits<std::uint32_t>::max() - header_size)
            return { { }, darwin::error::bad_address };
        const auto header = memory.read_bytes(address, header_size);
        if (!header)
            return { { }, darwin::error::bad_address };
        const auto word = [&header](std::size_t offset) {
            std::uint32_t value = 0;
            for (std::size_t byte = 0; byte < 4U; ++byte)
                value |= std::to_integer<std::uint32_t>((*header)[offset + byte])
                    << (byte * 8U);
            return value;
        };
        const auto count = word(36U);
        if (word(0U) != magic || (count != no_acl && count > maximum_entries))
            return { { }, darwin::error::invalid_argument };
        const auto size = header_size +
            (count == no_acl ? 0U : count) * entry_size;
        auto payload = memory.read_bytes(address, size);
        if (!payload)
            return { { }, darwin::error::bad_address };
        return { std::move(*payload), 0U };
    }

    [[nodiscard]] static std::uint32_t copy_out(AddressSpace& memory,
        std::uint32_t address, std::uint32_t size_address,
        const std::optional<std::vector<std::byte>>& stored)
    {
        if (address == 0U)
            return 0U;
        if (size_address == 0U)
            return darwin::error::bad_address;
        const auto capacity = memory.read32(size_address);
        if (!capacity)
            return darwin::error::bad_address;
        const auto size = stored ? static_cast<std::uint32_t>(stored->size()) : 0U;
        if (!memory.write32(size_address, size))
            return darwin::error::bad_address;
        if (stored && *capacity >= size && !memory.copy_to_user(address, *stored))
            return darwin::error::bad_address;
        return 0U;
    }
};

} // namespace ilemu::bsd::file_security
