#pragma once

#include <chrono>
#include <cstdint>

namespace ilemu {

// Relates Mach absolute time to a host monotonic clock for unbounded emulator
// sessions. Bounded test runs intentionally do not use this class so their
// virtual-time behavior remains deterministic and fast.
class RealtimePacer {
public:
  explicit RealtimePacer(std::uint64_t initial_virtual_time);

  [[nodiscard]] std::uint64_t allowed_virtual_time() const;
  [[nodiscard]] std::chrono::nanoseconds
  delay_until(std::uint64_t virtual_time) const;

private:
  std::uint64_t initial_virtual_time_{};
  std::chrono::steady_clock::time_point initial_host_time_;
};

} // namespace ilemu
