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

namespace kernel_iokit::baseband {

    inline constexpr std::string_view service_class { "AppleBaseband" };
    inline constexpr std::string_view serial_multiplexer_class {
        "AppleSerialMultiplexer"
    };
    inline constexpr std::string_view registry_name { "baseband" };

    enum class ServiceProfile {
        Baseband,
        SerialMultiplexer,
    };

    enum class SerialMultiplexerSelector : std::uint32_t {
        Configure = 0,
        GetTime = 2,
    };

    struct MethodResult {
        std::uint32_t return_code { };
        std::vector<std::uint64_t> scalar_output;
    };

    [[nodiscard]] std::optional<ServiceProfile> matching_service(
        std::span<const std::byte> matching);

    // The caller holds KernelSharedState::mach_mutex.
    [[nodiscard]] std::uint32_t ensure_service_locked(
        KernelSharedState& state, ServiceProfile profile);

    [[nodiscard]] std::optional<MethodResult> dispatch_connect_method(
        KernelSharedState& state, const ProcessContext& process,
        std::uint32_t connection_object, std::uint32_t selector,
        std::span<const std::uint64_t> scalar_input,
        std::span<const std::byte> inband_input,
        std::uint32_t scalar_output_capacity);

} // namespace kernel_iokit::baseband
} // namespace ilemu
