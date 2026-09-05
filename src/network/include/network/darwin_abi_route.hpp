#pragma once

#include <cstdint>

namespace ilemu {

enum class DarwinAbiEpoch {
    Unknown,
    IphoneOs1,
    IphoneOs2,
    IphoneOs3,
    Darwin10,
    Darwin11,
    Later,
};

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
    DarwinAbiEpoch minimum_epoch { DarwinAbiEpoch::Unknown };
    DarwinAbiEpoch maximum_epoch { DarwinAbiEpoch::Unknown };
};

// Syscall 322 changes from nosys to iopolicysys in xnu-1228.15.4 and remains
// iopolicysys in xnu-4903.241.1. The call number and wire shape are identical,
// so this is a genuine VersionSensitive collision rather than a
// shape-dispatched route. The dispatcher still accepts only the audited
// three-word disk shape; later XNU iotypes and policies are rejected there.
inline constexpr DarwinAbiRoute legacy_iopolicysys_route {
    DarwinAbiDomain::BsdSyscall, 322U, DarwinAbiCompatibility::VersionSensitive,
    DarwinAbiEpoch::IphoneOs2, DarwinAbiEpoch::Later
};

// The compatibility kernel historically made writable ARM heap pages
// executable after the i-cache fast trap so the first-generation UIKit
// trampoline path could run without an explicit mprotect.  XNU's cache trap
// itself does not change VM permissions, so keep this emulator-only behavior
// as an explicit epoch-bounded route instead of exposing it to later guests.
inline constexpr DarwinAbiRoute arm_cache_trap_execute_route {
    DarwinAbiDomain::ArmFastTrap,
    0U, // ARM fast-trap sub-operation: instruction_cache_invalidate
    DarwinAbiCompatibility::VersionSensitive, DarwinAbiEpoch::IphoneOs1,
    DarwinAbiEpoch::IphoneOs1
};

[[nodiscard]] constexpr bool darwin_abi_route_supported(
    const DarwinAbiRoute& route, DarwinAbiEpoch epoch)
{
    switch (route.compatibility) {
    case DarwinAbiCompatibility::Additive:
        return true;
    case DarwinAbiCompatibility::ShapeDispatched:
        // The route is epoch-independent, but its caller must validate the
        // argument shape before dispatching the implementation-specific ABI.
        return true;
    case DarwinAbiCompatibility::VersionSensitive:
        // Unknown builds must never opt into an ambiguous version-sensitive
        // collision, even if a future route happens to use Unknown as a bound.
        if (epoch == DarwinAbiEpoch::Unknown)
            return false;
        return static_cast<std::uint8_t>(epoch) >=
                   static_cast<std::uint8_t>(route.minimum_epoch) &&
               static_cast<std::uint8_t>(epoch) <=
                   static_cast<std::uint8_t>(route.maximum_epoch);
    case DarwinAbiCompatibility::Unsupported:
        return false;
    }
    return false;
}

} // namespace ilemu
