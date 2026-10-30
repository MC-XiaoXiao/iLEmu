// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// Describe version bounds for Darwin syscall and interface contracts.
//
// Apple public ABI/behavior references (guest profiles may differ):
// https://github.com/apple-oss-distributions/xnu/blob/xnu-792.24.17/bsd/kern/syscalls.master
// https://github.com/apple-oss-distributions/xnu/blob/xnu-1699.22.73/bsd/kern/syscalls.master

#pragma once

#include "foundation/darwin_version.hpp"
#include <cstdint>

namespace ilemu {

enum class DarwinAbiDomain {
    BsdSyscall,
    MachTrap,
    ArmFastTrap,
    MigRoutine,
};

enum class DarwinAbiCompatibility {
    Additive,
    ShapeDispatched,
    VersionSensitive,
    Unsupported,
};

struct DarwinAbiRoute {
    DarwinAbiDomain domain { };
    std::uint32_t identifier { };
    DarwinAbiCompatibility compatibility {
        DarwinAbiCompatibility::Unsupported
    };
    DarwinVersion minimum_version { DarwinGeneration::Unknown };
    DarwinVersion maximum_version { DarwinGeneration::Unknown };
};

// The disk-policy ABI assigns iopolicysys to a slot reserved as nosys by
// the legacy ABI. The call number cannot identify which contract is active,
// so this route uses the firmware major generation. The dispatcher accepts only
// the audited three-word disk shape; extended iotypes and policies are
// rejected.
inline constexpr DarwinAbiRoute legacy_iopolicysys_route {
    DarwinAbiDomain::BsdSyscall, 322U, DarwinAbiCompatibility::VersionSensitive,
    DarwinGeneration::IphoneOs2, DarwinGeneration::Ios10
};

// XNU's guarded descriptor update syscall is present in the Darwin 13 syscall
// table and later. Older profiles keep the slot on the normal nosys path.
inline constexpr DarwinAbiRoute guarded_fd_change_route {
    DarwinAbiDomain::BsdSyscall, 444U, DarwinAbiCompatibility::VersionSensitive,
    DarwinGeneration::Ios6, DarwinGeneration::Ios10
};

// XNU's socket connectx syscall is part of the Darwin 13 network ABI.
inline constexpr DarwinAbiRoute connectx_route { DarwinAbiDomain::BsdSyscall,
    447U, DarwinAbiCompatibility::VersionSensitive, DarwinGeneration::Ios6,
    DarwinGeneration::Ios10 };

// The compatibility kernel historically made writable ARM heap pages
// executable after the i-cache fast trap so the first-generation UIKit
// trampoline path could run without an explicit mprotect.  XNU's cache trap
// itself does not change VM permissions, so keep this emulator-only behavior
// as an explicit version-bounded route instead of exposing it to later guests.
inline constexpr DarwinAbiRoute arm_cache_trap_execute_route {
    DarwinAbiDomain::ArmFastTrap,
    0U, // ARM fast-trap sub-operation: instruction_cache_invalidate
    DarwinAbiCompatibility::VersionSensitive, DarwinGeneration::IphoneOs1,
    DarwinGeneration::IphoneOs1
};

[[nodiscard]] constexpr bool darwin_version_route_supported(
    const DarwinAbiRoute& route, DarwinVersion version)
{
    switch (route.compatibility) {
    case DarwinAbiCompatibility::Additive:
        return true;
    case DarwinAbiCompatibility::ShapeDispatched:
        // The route is version-independent, but its caller must validate the
        // argument shape before dispatching the implementation-specific ABI.
        return true;
    case DarwinAbiCompatibility::VersionSensitive:
        // Unknown builds must never opt into an ambiguous version-sensitive
        // collision, even if a future route happens to use Unknown as a bound.
        if (!version.known())
            return false;
        return version >= route.minimum_version &&
               version <= route.maximum_version;
    case DarwinAbiCompatibility::Unsupported:
        return false;
    }
    return false;
}

} // namespace ilemu
