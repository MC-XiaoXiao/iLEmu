// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#pragma once

#include "kernel/darwin_abi.hpp"
#include "network/darwin_abi_route.hpp"

namespace ilemu::bsd_vm {

// kern_mman.c validates these before its zero-length return. Anonymous
// descriptors carry allocation flags from XNU1228; mmap accepts superpage
// selectors and defines MAP_JIT from XNU1699 (later than vm_allocate).
class MappingOptions {
public:
    explicit constexpr MappingOptions(DarwinAbiEpoch epoch) : epoch_(epoch) { }

    constexpr bool valid(std::uint32_t flags, std::uint32_t descriptor) const
    {
        const auto anonymous = (flags & darwin::map_flag::anonymous) != 0U;
        constexpr auto jit = 0x0800U;
        if (epoch_ >= DarwinAbiEpoch::Darwin11 && (flags & jit) != 0U &&
            ((flags & (darwin::map_flag::fixed | darwin::map_flag::shared)) != 0U ||
                (epoch_ >= DarwinAbiEpoch::Darwin13 && !anonymous)))
            return false;
        if (!anonymous || descriptor == UINT32_MAX)
            return true;
        if (epoch_ < DarwinAbiEpoch::IphoneOs2)
            return false;
        auto allowed = 0xff000002U; // VM_FLAGS_ALIAS_MASK | VM_FLAGS_PURGABLE
        if (epoch_ >= DarwinAbiEpoch::Darwin11)
            allowed |= 0x00070000U; // VM_FLAGS_SUPERPAGE_MASK
        return (descriptor & ~allowed) == 0U;
    }

private:
    DarwinAbiEpoch epoch_;
};

} // namespace ilemu::bsd_vm
