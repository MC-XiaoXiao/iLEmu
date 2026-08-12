#pragma once

#include <cstddef>
#include <filesystem>
#include <span>
#include <vector>

namespace ilemu::bsd::baseband_device {

[[nodiscard]] std::vector<std::byte>
load_replay_file(const std::filesystem::path &path);
void write_capture_file(const std::filesystem::path &path,
                        std::span<const std::byte> bytes);

} // namespace ilemu::bsd::baseband_device
