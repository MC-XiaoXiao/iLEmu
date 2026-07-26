#include "ilemu/realtime_pacer.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <limits>

namespace ilemu {

RealtimePacer::RealtimePacer(std::uint64_t initial_virtual_time)
    : initial_virtual_time_{initial_virtual_time},
      initial_host_time_{std::chrono::steady_clock::now()} {}

std::uint64_t RealtimePacer::allowed_virtual_time() const {
  // iPhone OS 1.0's mach_timebase_info is exposed as 1:1, so one Mach
  // absolute-time unit is one nanosecond in the compatibility kernel.
  const auto elapsed = std::max(
      std::chrono::steady_clock::now() - initial_host_time_,
      std::chrono::steady_clock::duration::zero());
  const auto elapsed_nanoseconds =
      std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count();
  const auto positive_elapsed = static_cast<std::uint64_t>(elapsed_nanoseconds);
  if (positive_elapsed >
      std::numeric_limits<std::uint64_t>::max() - initial_virtual_time_) {
    return std::numeric_limits<std::uint64_t>::max();
  }
  return initial_virtual_time_ + positive_elapsed;
}

std::chrono::nanoseconds
RealtimePacer::delay_until(
    std::uint64_t virtual_time,
    std::optional<std::chrono::steady_clock::time_point> host_deadline) const {
  const auto allowed = allowed_virtual_time();
  if (virtual_time <= allowed) {
    return std::chrono::nanoseconds::zero();
  }
  const auto delay = virtual_time - allowed;
  const auto maximum = static_cast<std::uint64_t>(
      std::chrono::nanoseconds::max().count());
  return limit_delay(
      std::chrono::nanoseconds{static_cast<std::chrono::nanoseconds::rep>(
          std::min(delay, maximum))},
      host_deadline);
}

std::chrono::nanoseconds RealtimePacer::limit_delay(
    std::chrono::nanoseconds delay,
    std::optional<std::chrono::steady_clock::time_point> host_deadline) const {
  if (delay <= std::chrono::nanoseconds::zero() || !host_deadline)
    return std::max(delay, std::chrono::nanoseconds::zero());
  const auto until_host_deadline = std::chrono::duration_cast<
      std::chrono::nanoseconds>(*host_deadline - std::chrono::steady_clock::now());
  return std::min(delay,
                  std::max(until_host_deadline,
                           std::chrono::nanoseconds::zero()));
}

} // namespace ilemu
