#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

namespace ilemu {

struct DarwinKernelIdentityProfile {
  std::string name{"darwin9.4"};
  std::string operating_system_type{"Darwin"};
  std::string operating_system_release{"9.4.0"};
  std::uint32_t operating_system_revision{199506};
  std::string version{
      "Darwin Kernel Version 9.4.0: iLEmu compatibility kernel; "
      "darwin9.4/RELEASE_ARM"};
  std::string build_version{"1A543a"};
};

// Reports the compatibility kernel's highest supported Darwin contract. The
// firmware metadata contributes only its descriptive build identifier and
// never selects kernel behavior.
[[nodiscard]] DarwinKernelIdentityProfile
make_darwin_kernel_identity_profile(const std::filesystem::path &rootfs);

} // namespace ilemu
