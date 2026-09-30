// SPDX-License-Identifier: MPL-2.0
#pragma once
#include <filesystem>
#include <optional>
#include <string>
namespace ilemu {
class Output;
void inspect_mig(const std::optional<std::filesystem::path>& rootfs,
    const std::optional<std::string>& ios_build, Output& output, bool all,
    bool validate_only);
}
