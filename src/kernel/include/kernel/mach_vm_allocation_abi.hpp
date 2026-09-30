// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
#pragma once

#include "network/darwin_abi_route.hpp"
#include <cstdint>

namespace ilemu::darwin::mach::vm_allocation {

// vm_statistics.h: USER_ALLOCATE appears in XNU1228, the superpage mask
// in XNU1456, and user OVERWRITE in XNU1699. vm_map_enter rejects nonzero
// superpage selectors outside x86_64; vm_user checks user flags before size.
class Contract {
public:
    explicit constexpr Contract(DarwinAbiEpoch epoch) : epoch_(epoch) { }

    constexpr std::uint32_t user_flag_mask() const
    {
        if (epoch_ < DarwinAbiEpoch::IphoneOs2)
            return UINT32_MAX; // XNU792 has no user flag filter.
        auto mask = 0xff000013U; // alias, no-cache, purgeable, anywhere
        if (epoch_ >= DarwinAbiEpoch::IphoneOs3)
            mask |= superpage_mask;
        if (epoch_ >= DarwinAbiEpoch::Darwin11)
            mask |= overwrite_flag;
        return mask;
    }
    constexpr bool valid_user_flags(std::uint32_t flags) const
    {
        return (flags & ~user_flag_mask()) == 0U;
    }
    constexpr bool valid_mapping_flags(std::uint32_t flags) const
    {
        return epoch_ < DarwinAbiEpoch::IphoneOs3 ||
               (flags & superpage_mask) == 0U;
    }
    constexpr bool replaces_mapping(std::uint32_t flags) const
    {
        return epoch_ >= DarwinAbiEpoch::Darwin11 &&
               (flags & overwrite_flag) != 0U && (flags & 1U) == 0U;
    }

private:
    static constexpr std::uint32_t superpage_mask = 0x00070000U;
    static constexpr std::uint32_t overwrite_flag = 0x00004000U;
    DarwinAbiEpoch epoch_;
};

} // namespace ilemu::darwin::mach::vm_allocation
