#pragma once

#include <chrono>
#include <deque>
#include <optional>
#include <vector>

#include "ilemu/system_button_input.hpp"

namespace ilemu {

// Keeps a physical button Down event alive for a host-controlled duration and
// emits the matching Up event without involving the SDL window or host GUI.
class LiveButtonScheduler {
public:
  void schedule(SystemButtonInput down, std::chrono::milliseconds hold);
  [[nodiscard]] std::vector<SystemButtonInput> poll();
  [[nodiscard]] bool empty() const { return events_.empty(); }
  [[nodiscard]] std::optional<std::chrono::steady_clock::time_point>
  next_deadline() const;

private:
  struct Event {
    std::chrono::steady_clock::time_point deadline;
    SystemButtonInput input;
  };

  std::deque<Event> events_;
};

} // namespace ilemu
