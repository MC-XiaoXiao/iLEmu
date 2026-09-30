// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#pragma once

#include "foundation/vm_map.hpp"
#include "kernel/darwin_abi.hpp"
#include "network/darwin_abi_route.hpp"

namespace ilemu::bsd_vm {

// kern_mman.c in XNU1228+ implies READ for WRITE/EXECUTE (3777787,
// 3936456); XNU792 retains the requested bits. This conversion belongs
// to BSD mmap/mprotect, not the underlying Mach protection operation.
class Protection {
public:
    explicit constexpr Protection(DarwinAbiEpoch epoch) : epoch_(epoch) { }

    constexpr std::uint32_t normalized_bits(std::uint32_t protection) const
    {
        auto bits = protection & 7U;
        if (epoch_ >= DarwinAbiEpoch::IphoneOs2 && (bits & 6U) != 0U)
            bits |= 1U;
        return bits;
    }

    constexpr MemoryPermission permissions(std::uint32_t protection) const
    {
        return static_cast<MemoryPermission>(normalized_bits(protection));
    }

    enum class Object { File, PosixSharedMemory };
    struct Mapping {
        VmMappingAttributes attributes;
        std::uint32_t error { };
    };

    // kern_mman.c derives file limits from FREAD/FWRITE. posix_shm.c
    // instead maps a named object capped at VM_PROT_DEFAULT (READ|WRITE),
    // requires MAP_SHARED, and reports EPERM for a read-only write request.
    constexpr Mapping file_mapping(MemoryPermission current,
        std::uint32_t descriptor_flags, bool shared, Object object) const
    {
        Mapping result;
        result.attributes.inheritance =
            shared ? VmInheritance::Share : VmInheritance::Copy;
        const auto access = descriptor_flags & darwin::open_flag::access_mode;
        const auto readable = access == darwin::open_flag::read_only ||
                              access == darwin::open_flag::read_write;
        const auto writable = access == darwin::open_flag::write_only ||
                              access == darwin::open_flag::read_write;
        if (object == Object::PosixSharedMemory) {
            result.attributes.maximum_permissions =
                MemoryPermission::Read | MemoryPermission::Write;
            if (!shared)
                result.error = darwin::error::invalid_argument;
            else if (!writable && has_permission(current, MemoryPermission::Write))
                result.error = 1U; // EPERM, before the named-object check.
            else if (has_permission(current, MemoryPermission::Execute))
                result.error = darwin::error::invalid_argument;
            return result;
        }
        auto maximum = MemoryPermission::Execute;
        if (readable)
            maximum |= MemoryPermission::Read;
        else if (has_permission(current, MemoryPermission::Read))
            result.error = darwin::error::permission_denied;
        if (!shared || writable)
            maximum |= MemoryPermission::Write;
        else if (has_permission(current, MemoryPermission::Write))
            result.error = darwin::error::permission_denied;
        result.attributes.maximum_permissions =
            permissions(static_cast<std::uint32_t>(maximum));
        return result;
    }

private:
    DarwinAbiEpoch epoch_;
};

} // namespace ilemu::bsd_vm
