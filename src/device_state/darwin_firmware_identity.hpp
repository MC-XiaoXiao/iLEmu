#pragma once

#include <filesystem>
#include <string>

namespace ilemu {

[[nodiscard]] std::string read_darwin_build_version(
    const std::filesystem::path& rootfs);

} // namespace ilemu
