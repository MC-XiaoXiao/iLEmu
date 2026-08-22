#pragma once

#include <chrono>
#include <cstdint>
#include <optional>

namespace ilemu {

// Mach absolute time is the guest DeviceMonotonicTime domain. Execution
// accounting advances that domain through the scheduler; this mapping only
// answers what device time the host steady clock permits. It never rebases the
// mapping when guest execution is slow.
using DeviceMonotonicTime = std::uint64_t;

// Relates guest DeviceMonotonicTime to one fixed host steady-clock origin for
// interactive emulator sessions. Bounded test runs intentionally do not use
// this class so their deterministic-time behavior remains fast and repeatable.
class RealtimePacer {
public:
  explicit RealtimePacer(DeviceMonotonicTime initial_device_monotonic_time);

  [[nodiscard]] DeviceMonotonicTime
  allowed_device_monotonic_time() const;
  [[nodiscard]] std::chrono::nanoseconds
  delay_until(DeviceMonotonicTime device_monotonic_time) const;
  // Return the stable host deadline corresponding to a future device time.
  // Unlike now()+delay_until(), this does not drift when the caller refreshes
  // the same guest deadline on every idle-loop iteration.
  [[nodiscard]] std::chrono::steady_clock::time_point
  host_deadline_for(DeviceMonotonicTime device_monotonic_time) const;
  [[nodiscard]] std::chrono::nanoseconds limit_delay(
      std::chrono::nanoseconds delay,
      std::optional<std::chrono::steady_clock::time_point> host_deadline) const;

private:
  DeviceMonotonicTime initial_device_monotonic_time_{};
  std::chrono::steady_clock::time_point initial_host_time_;
};

} // namespace ilemu
