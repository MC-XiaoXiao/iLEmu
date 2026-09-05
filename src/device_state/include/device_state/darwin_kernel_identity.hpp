#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace ilemu {

struct DarwinKernelIdentity {
    explicit DarwinKernelIdentity(std::string_view darwin_release = "9.0.0d1",
        std::string_view ios_build = "1A543a");

    std::string name;
    std::string operating_system_type { "Darwin" };
    std::string operating_system_release;
    // kern.osrevision is the BSD compatibility revision, not a Darwin release.
    std::uint32_t operating_system_revision { 199506 };
    std::string version;
    std::string build_version;
};

} // namespace ilemu
