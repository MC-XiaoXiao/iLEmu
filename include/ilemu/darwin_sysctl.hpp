#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace ilemu::darwin::sysctl {

inline constexpr std::uint32_t control_unspecified = 0;
inline constexpr std::uint32_t control_kernel = 1;
inline constexpr std::uint32_t control_hardware = 6;
inline constexpr std::uint32_t operation_name_to_oid = 3;
inline constexpr std::uint32_t kernel_process = 14;
inline constexpr std::uint32_t kernel_process_arguments = 38;
inline constexpr std::uint32_t kernel_process_arguments2 = 49;
inline constexpr std::uint32_t kernel_process_all = 0;
inline constexpr std::uint32_t kernel_process_id = 1;
inline constexpr std::uint32_t arm32_kernel_process_info_size = 492;

inline constexpr std::uint32_t hardware_machine = 1;
inline constexpr std::uint32_t hardware_model = 2;

struct ObjectIdentifier {
  std::array<std::uint32_t, 2> components{};
  std::size_t size{};
};

// Resolves the fixed Darwin 8 nodes currently projected by the compatibility
// kernel. Dynamic OID_AUTO nodes can be added here as their values are exposed.
[[nodiscard]] std::optional<ObjectIdentifier>
resolve_name(std::string_view name);

// Returns the selected device profile string projected by a CTL_HW selector.
[[nodiscard]] std::optional<std::string_view>
hardware_string(std::uint32_t selector, std::string_view machine,
                std::string_view model);

// Encodes the stable prefix consumed by Darwin 8 KERN_PROCARGS clients:
// executable path, word alignment, argv strings, then environment strings.
[[nodiscard]] std::vector<std::byte> encode_process_arguments(
    std::string_view executable_path,
    std::span<const std::string> arguments,
    std::span<const std::string> environment);

// KERN_PROCARGS2 inserts argc between the aligned executable path and argv.
[[nodiscard]] std::vector<std::byte> encode_process_arguments2(
    std::string_view executable_path,
    std::span<const std::string> arguments,
    std::span<const std::string> environment);

} // namespace ilemu::darwin::sysctl
