#pragma once

#include <chrono>
#include <cstdint>
#include <optional>

namespace ilemu {

// Mach absolute time is the guest DeviceMonotonicTime domain. Interactive
// execution maps it to host steady time; instruction ticks remain a separate
// CPU/scheduler accounting domain and must not advance this clock.
using DeviceMonotonicTime = std::uint64_t;

enum class DeviceTimePolicy : std::uint8_t {
    DeterministicExecution,
    HostMappedInteractive,
};

// Relates guest DeviceMonotonicTime to one fixed host steady-clock origin for
// interactive emulator sessions. Bounded test runs intentionally do not use
// this class so their deterministic-time behavior remains fast and repeatable.
class RealtimePacer {
public:
    explicit RealtimePacer(DeviceMonotonicTime initial_device_monotonic_time);

    [[nodiscard]] DeviceMonotonicTime allowed_device_monotonic_time() const;
    [[nodiscard]] std::chrono::nanoseconds delay_until(
        DeviceMonotonicTime device_monotonic_time) const;
    // Return the stable host deadline corresponding to a future device time.
    // Unlike now()+delay_until(), this does not drift when the caller refreshes
    // the same guest deadline on every idle-loop iteration.
    [[nodiscard]] std::chrono::steady_clock::time_point host_deadline_for(
        DeviceMonotonicTime device_monotonic_time) const;
    [[nodiscard]] std::chrono::nanoseconds limit_delay(
        std::chrono::nanoseconds delay,
        std::optional<std::chrono::steady_clock::time_point> host_deadline)
        const;

private:
    DeviceMonotonicTime initial_device_monotonic_time_ { };
    std::chrono::steady_clock::time_point initial_host_time_;
};

} // namespace ilemu
