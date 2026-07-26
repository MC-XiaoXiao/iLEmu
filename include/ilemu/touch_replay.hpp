#pragma once

#include <chrono>
#include <filesystem>
#include <optional>
#include <vector>

#include "ilemu/touch_input.hpp"

namespace ilemu {

// Host-time replay keeps UI automation independent of guest scheduling speed.
// Each text line is: <delay-ms> <down|move|up|cancel> <x> <y>.
class TouchReplay {
public:
  explicit TouchReplay(const std::filesystem::path &path);

  void start();
  [[nodiscard]] std::vector<TouchInput> poll();
  [[nodiscard]] bool empty() const { return next_event_ >= events_.size(); }
  [[nodiscard]] bool settled(std::chrono::milliseconds quiet_period) const;
  [[nodiscard]] std::optional<std::chrono::steady_clock::time_point>
  next_deadline() const;

private:
  struct Event {
    std::chrono::milliseconds delay{};
    TouchInput input;
  };

  std::vector<Event> events_;
  std::chrono::steady_clock::time_point start_time_{};
  std::size_t next_event_{};
  bool started_{};
};

} // namespace ilemu
