#pragma once

#include <cstdint>
#include <string>

namespace ilemu {

struct DarwinKernelIdentity {
    std::string name { "darwin9.4" };
    std::string operating_system_type { "Darwin" };
    std::string operating_system_release { "9.4.0" };
    std::uint32_t operating_system_revision { 199506 };
    std::string version {
        "Darwin Kernel Version 9.4.0: iLEmu compatibility kernel; "
        "darwin9.4/RELEASE_ARM"
    };
    // Compatibility value exposed through kern.osversion/sysctl when the
    // firmware does not provide a trustworthy ProductBuildVersion.
    std::string build_version { "1A543a" };
};

} // namespace ilemu
