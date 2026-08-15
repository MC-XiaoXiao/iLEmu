#pragma once

#include <cstddef>
#include <filesystem>
#include <span>
#include <vector>

namespace ilemu::bsd::baseband_device {

inline constexpr std::size_t maximum_replay_bytes = 64U * 1024U * 1024U;

[[nodiscard]] std::vector<std::byte>
load_replay_file(const std::filesystem::path &path);
void write_capture_file(const std::filesystem::path &path,
                        std::span<const std::byte> bytes);

} // namespace ilemu::bsd::baseband_device
