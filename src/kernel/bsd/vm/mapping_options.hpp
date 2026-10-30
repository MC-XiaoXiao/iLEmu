// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#pragma once

#include "kernel/darwin_abi.hpp"
#include "network/darwin_version_route.hpp"

namespace ilemu::bsd_vm {

// kern_mman.c validates these before its zero-length return. Anonymous
// descriptors carry byte-sized memory tags in early ARM libc, then Mach
// allocation flags in later clients; mmap accepts superpage
// selectors and defines MAP_JIT from XNU1699 (later than vm_allocate).
class MappingOptions {
public:
    explicit constexpr MappingOptions(DarwinVersion version)
        : version_(version)
    {
    }

    constexpr bool valid(std::uint32_t flags, std::uint32_t descriptor) const
    {
        const auto anonymous = (flags & darwin::map_flag::anonymous) != 0U;
        constexpr auto jit = 0x0800U;
        if ((version_ >= DarwinGeneration::Ios4) && (flags & jit) != 0U &&
            ((flags & (darwin::map_flag::fixed | darwin::map_flag::shared)) !=
                    0U ||
                ((version_ >= DarwinGeneration::Ios6) && !anonymous)))
            return false;
        if (!anonymous || descriptor == UINT32_MAX)
            return true;
        if (anonymous_descriptor_format() == AnonymousDescriptorFormat::MemoryTag)
            return descriptor <= 0xffU;
        auto allowed = 0xff000002U; // VM_FLAGS_ALIAS_MASK | VM_FLAGS_PURGABLE
        if (version_ >= DarwinGeneration::Ios4)
            allowed |= 0x00070000U; // VM_FLAGS_SUPERPAGE_MASK
        return (descriptor & ~allowed) == 0U;
    }

private:
    enum class AnonymousDescriptorFormat { MemoryTag, AllocationFlags };

    constexpr AnonymousDescriptorFormat anonymous_descriptor_format() const
    {
        // Early ARM malloc passes VM_MEMORY_* directly (for example, tag 1
        // when creating a scalable zone). Later clients use VM_MAKE_TAG and
        // can combine it with the permitted Mach VM allocation flags.
        return (version_ < DarwinGeneration::IphoneOs2)
                   ? AnonymousDescriptorFormat::MemoryTag
                   : AnonymousDescriptorFormat::AllocationFlags;
    }

    DarwinVersion version_;
};

} // namespace ilemu::bsd_vm
