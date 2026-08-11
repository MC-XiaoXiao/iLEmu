#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

#include "ilemu/darwin_abi_route.hpp"

namespace ilemu {

struct DarwinGuestCapabilities {
  // XNU's nosys entry returns ENOSYS and raises SIGSYS on the audited
  // production epochs. Unknown profiles conservatively suppress the signal
  // until their kernel policy is identified.
  bool send_sigsys{};
  // Early UIKit emits writable ARM trampolines and relies on the emulator's
  // compatibility permission promotion after the instruction-cache trap.
  // This is a version-sensitive capability; later and unknown profiles keep
  // the XNU behavior of leaving VM permissions unchanged.
  bool arm_cache_trap_grants_execute{};
};

struct DarwinKernelIdentityProfile {
  std::string name{"darwin9.4"};
  std::string operating_system_type{"Darwin"};
  std::string operating_system_release{"9.4.0"};
  std::uint32_t operating_system_revision{199506};
  std::string version{
      "Darwin Kernel Version 9.4.0: iLEmu compatibility kernel; "
      "darwin9.4/RELEASE_ARM"};
  std::string build_version{"1A543a"};
  // The firmware build is the identity we read from SystemVersion.plist;
  // this independently names the audited Darwin/XNU ABI epoch. Keeping it
  // outside capabilities prevents a new capability from accidentally
  // changing version-sensitive route selection.
  DarwinAbiEpoch abi_epoch{DarwinAbiEpoch::Unknown};
  DarwinGuestCapabilities capabilities;
};

// Reports the compatibility kernel's highest supported Darwin contract. The
// firmware build is also available to explicitly audited ABI dispatch points;
// unknown builds retain the conservative compatibility behavior.
[[nodiscard]] DarwinKernelIdentityProfile
make_darwin_kernel_identity_profile(const std::filesystem::path &rootfs);

} // namespace ilemu
