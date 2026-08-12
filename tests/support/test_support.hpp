#pragma once

#include <filesystem>
#include <span>
#include <string>
#include <string_view>

namespace ilemu::test {

void require(bool condition, std::string_view message);

[[nodiscard]] std::filesystem::path find_existing(
    std::span<const std::filesystem::path> candidates);

[[nodiscard]] std::string read_binary_file(
    const std::filesystem::path& path);

int run_suite(std::string_view name, void (*suite)());

}  // namespace ilemu::test
