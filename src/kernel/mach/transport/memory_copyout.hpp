// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#pragma once

#include "../support.hpp"
#include "foundation/address_space.hpp"
#include <limits>
#include <optional>
#include <span>

namespace ilemu::mach_ipc {
// Both queued and direct kernel replies install each OOL VM copy in the
// actual receiver map. Separate payloads must remain independently unmapable.
inline std::optional<std::uint32_t> copyout_memory(
    AddressSpace& memory, std::span<const std::byte> bytes)
{
    if (bytes.empty())
        return 0U;
    const auto size =
        (static_cast<std::uint64_t>(bytes.size()) + AddressSpace::page_size -
            1U) &
        ~(static_cast<std::uint64_t>(AddressSpace::page_size) - 1U);
    if (size > std::numeric_limits<std::uint32_t>::max())
        return std::nullopt;
    const auto mapped_size = static_cast<std::uint32_t>(size);
    const auto address = mach_support::find_free_guest_region(
        memory, mach_support::ool_receive_base, mapped_size);
    if (!address ||
        !memory.map(*address, mapped_size,
            MemoryPermission::Read | MemoryPermission::Write) ||
        !memory.copy_in(*address, bytes))
        return std::nullopt;
    return address;
}
} // namespace ilemu::mach_ipc
