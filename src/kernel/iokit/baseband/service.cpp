#include "ilemu/kernel_iokit_baseband.hpp"

#include "ilemu/iokit_abi.hpp"
#include "ilemu/kernel_shared_state.hpp"

#include <algorithm>
#include <limits>
#include <mutex>
#include <string>

namespace ilemu::kernel_iokit::baseband {

namespace {

    bool contains(std::span<const std::byte> matching, std::string_view value)
    {
        return std::search(matching.begin(), matching.end(), value.begin(),
                   value.end(), [](std::byte byte, char character) {
                       return std::to_integer<unsigned char>(byte) ==
                              static_cast<unsigned char>(character);
                   }) != matching.end();
    }

} // namespace

std::optional<ServiceProfile> matching_service(
    std::span<const std::byte> matching)
{
    if (contains(matching, serial_multiplexer_class))
        return ServiceProfile::SerialMultiplexer;
    if (contains(matching, service_class) || contains(matching, registry_name))
        return ServiceProfile::Baseband;
    return std::nullopt;
}

std::uint32_t ensure_service_locked(
    KernelSharedState& state, ServiceProfile profile)
{
    auto& cached_object = profile == ServiceProfile::SerialMultiplexer
                              ? state.serial_multiplexer_service
                              : state.baseband_service;
    if (cached_object != 0)
        return cached_object;

    const auto object = state.allocate_mach_object();
    cached_object = object;
    static_cast<void>(state.mach_port_objects.create(object));
    state.mach_queues.try_emplace(object);
    const auto serial_multiplexer =
        profile == ServiceProfile::SerialMultiplexer;
    state.iokit_services.emplace(object,
        KernelSharedState::IOKitService {
            std::string {
                serial_multiplexer ? serial_multiplexer_class : service_class },
            { "IOService" }, { }, { }, 0,
            serial_multiplexer
                ? KernelSharedState::IOKitUserClientProfile::SerialMultiplexer
                : KernelSharedState::IOKitUserClientProfile::Generic });
    return object;
}

std::optional<MethodResult> dispatch_connect_method(KernelSharedState& state,
    const ProcessContext& process, std::uint32_t connection_object,
    std::uint32_t selector, std::span<const std::uint64_t> scalar_input,
    std::span<const std::byte> inband_input,
    std::uint32_t scalar_output_capacity)
{
    constexpr std::uint64_t nanoseconds_per_second = 1'000'000'000ULL;
    constexpr std::uint64_t nanoseconds_per_microsecond = 1'000ULL;

    std::lock_guard lock { state.mach_mutex };
    const auto connection = state.iokit_connections.find(connection_object);
    if (connection == state.iokit_connections.end() ||
        connection->second.owner_pid != process.pid) {
        return std::nullopt;
    }
    const auto service =
        state.iokit_services.find(connection->second.service_port);
    if (service == state.iokit_services.end() ||
        service->second.user_client_profile !=
            KernelSharedState::IOKitUserClientProfile::SerialMultiplexer) {
        return std::nullopt;
    }

    if (selector ==
        static_cast<std::uint32_t>(SerialMultiplexerSelector::Configure)) {
        // The native driver uses these two scalars to tune its physical mux.
        // The virtual baseband transport already owns queueing and framing, but
        // retain the user-client ABI's strict shape instead of treating
        // arbitrary selectors as successful.
        if (scalar_input.size() != 2U || !inband_input.empty() ||
            scalar_output_capacity != 0U || scalar_input[0] == 0U ||
            scalar_input[1] == 0U ||
            scalar_input[0] > std::numeric_limits<std::uint32_t>::max() ||
            scalar_input[1] > std::numeric_limits<std::uint32_t>::max()) {
            return MethodResult { iokit_abi::bad_argument, { } };
        }
        return MethodResult { iokit_abi::success, { } };
    }

    if (selector !=
        static_cast<std::uint32_t>(SerialMultiplexerSelector::GetTime))
        return MethodResult { iokit_abi::unsupported, { } };
    if (!scalar_input.empty() || !inband_input.empty() ||
        scalar_output_capacity < 2U)
        return MethodResult { iokit_abi::bad_argument, { } };

    // AppleSerialMultiplexer reports the kernel calendar as a timeval pair.
    // CommCenter converts seconds + microseconds/1000 into its millisecond
    // timeline, so source both fields from the same virtual calendar used by
    // gettimeofday instead of consulting the host clock independently.
    const auto wall_time = state.clock.wall_time();
    const auto seconds = wall_time / nanoseconds_per_second;
    const auto microseconds =
        (wall_time / nanoseconds_per_microsecond) % 1'000'000ULL;
    if (seconds > std::numeric_limits<std::uint32_t>::max())
        return MethodResult { iokit_abi::unsupported, { } };
    return MethodResult { iokit_abi::success, { seconds, microseconds } };
}

} // namespace ilemu::kernel_iokit::baseband
