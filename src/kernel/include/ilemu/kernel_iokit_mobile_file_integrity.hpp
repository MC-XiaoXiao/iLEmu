#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace ilemu {

struct KernelSharedState;
struct ProcessContext;

namespace kernel_iokit::mobile_file_integrity {

inline constexpr std::string_view service_class{
    "AppleMobileFileIntegrity"};

struct MethodResult {
  std::uint32_t return_code{};
  std::vector<std::byte> inband_output;
};

[[nodiscard]] bool matches_service(std::span<const std::byte> matching);

// The caller holds KernelSharedState::mach_mutex.
[[nodiscard]] std::uint32_t
ensure_service_locked(KernelSharedState &state);

[[nodiscard]] std::optional<MethodResult> dispatch_connect_method(
    KernelSharedState &state, const ProcessContext &process,
    std::uint32_t connection_object, std::uint32_t selector,
    std::span<const std::uint64_t> scalar_input,
    std::span<const std::byte> inband_input,
    std::uint32_t scalar_output_capacity,
    std::uint32_t inband_output_capacity);

} // namespace kernel_iokit::mobile_file_integrity
} // namespace ilemu
