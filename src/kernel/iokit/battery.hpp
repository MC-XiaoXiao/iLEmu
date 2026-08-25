#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace ilemu {

struct KernelSharedState;

namespace kernel_iokit::battery {

inline constexpr std::string_view service_class{"IOPMPowerSource"};

[[nodiscard]] bool matches_service(std::span<const std::byte> matching);

// The caller holds KernelSharedState::mach_mutex.
[[nodiscard]] std::uint32_t ensure_service_locked(KernelSharedState &state);

} // namespace kernel_iokit::battery
} // namespace ilemu
