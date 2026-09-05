#pragma once

#include <filesystem>
#include <optional>
#include <string>

namespace ilemu {

class Output;

void inspect_abi(const std::optional<std::filesystem::path>& rootfs,
    const std::optional<std::string>& ios_build, Output& output);

} // namespace ilemu
