#pragma once

#include <cstdint>

namespace ilemu {

enum class DarwinAbiEpoch {
  Unknown,
  IphoneOs1,
  IphoneOs2,
  IphoneOs3,
  Later,
};

enum class DarwinAbiDomain {
  BsdSyscall,
  MachTrap,
  MigRoutine,
};

enum class DarwinAbiCompatibility {
  Additive,
  ShapeDispatched,
  VersionSensitive,
  Unsupported,
};

struct DarwinAbiRoute {
  DarwinAbiDomain domain{};
  std::uint32_t identifier{};
  DarwinAbiCompatibility compatibility{DarwinAbiCompatibility::Unsupported};
  DarwinAbiEpoch minimum_epoch{DarwinAbiEpoch::Unknown};
  DarwinAbiEpoch maximum_epoch{DarwinAbiEpoch::Unknown};
};

// Syscall 322 changes from nosys to the old three-word disk iopolicysys
// contract. The call number and wire shape are identical, so this is a
// genuine VersionSensitive collision rather than a shape-dispatched route.
inline constexpr DarwinAbiRoute legacy_iopolicysys_route{
    DarwinAbiDomain::BsdSyscall,
    322U,
    DarwinAbiCompatibility::VersionSensitive,
    DarwinAbiEpoch::IphoneOs2,
    DarwinAbiEpoch::IphoneOs3};

[[nodiscard]] constexpr bool darwin_abi_route_supported(
    const DarwinAbiRoute &route, DarwinAbiEpoch epoch) {
  switch (route.compatibility) {
  case DarwinAbiCompatibility::Additive:
  case DarwinAbiCompatibility::ShapeDispatched:
    return true;
  case DarwinAbiCompatibility::VersionSensitive:
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
