#pragma once

#include <filesystem>
#include <optional>
#include <string>

namespace ilemu {

class Output;

void inspect_abi(const std::optional<std::filesystem::path>& rootfs,
    const std::optional<std::string>& requested_abi, Output& output);

} // namespace ilemu
