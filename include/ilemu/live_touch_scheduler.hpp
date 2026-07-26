#pragma once

#include <chrono>
#include <deque>
#include <optional>
#include <span>
#include <vector>

#include "ilemu/live_control.hpp"
#include "ilemu/touch_input.hpp"

namespace ilemu {

// Replays a complete live-control gesture against host steady time. Keeping
// this separate from command parsing makes multi-point gestures independent of
// terminal read chunking and guest scheduling speed.
class LiveTouchScheduler {
public:
  void schedule(std::span<const LiveTouchEvent> gesture);
  [[nodiscard]] std::vector<TouchInput> poll();
  [[nodiscard]] bool empty() const { return events_.empty(); }
  [[nodiscard]] std::optional<std::chrono::steady_clock::time_point>
  next_deadline() const;

private:
  struct Event {
    std::chrono::steady_clock::time_point deadline;
    TouchInput input;
  };

  std::deque<Event> events_;
};

} // namespace ilemu
