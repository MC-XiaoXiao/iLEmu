#pragma once

#include <chrono>
#include <filesystem>
#include <string>
#include <vector>

#include "ilemu/display_geometry.hpp"
#include "ilemu/system_button_input.hpp"
#include "ilemu/touch_input.hpp"

namespace ilemu {

enum class LiveControlCommandKind {
  Touch,
  Gesture,
  Button,
  ButtonHold,
  Home,
  Lock,
  VolumeUp,
  VolumeDown,
  RingerRing,
  RingerSilent,
  Snapshot,
  SnapshotSequence,
  PerfBegin,
  PerfEnd,
  Status,
  Help,
  Quit,
  Error,
};

struct LiveTouchEvent {
  std::chrono::milliseconds delay{};
  TouchInput input;
};

struct LiveControlCommand {
  LiveControlCommandKind kind{LiveControlCommandKind::Error};
  TouchInput touch;
  std::vector<LiveTouchEvent> gesture;
  SystemButtonInput system_button;
  std::chrono::milliseconds button_hold{};
  bool wake_display{};
  std::filesystem::path path;
  std::chrono::milliseconds snapshot_interval{};
  std::size_t snapshot_count{};
  std::string message;
};

// Non-blocking line-oriented control channel used by headless interactive
// sessions. The descriptor remains owned by the caller.
class LiveControl {
public:
  explicit LiveControl(int descriptor,
                       DisplayGeometry geometry = default_display_geometry);

  [[nodiscard]] std::vector<LiveControlCommand> poll();
  // Blocks until the descriptor is readable/hung up or the timeout expires.
  // The next poll() still owns buffering and command parsing.
  void wait_for(std::chrono::nanoseconds timeout);
  [[nodiscard]] bool closed() const { return closed_; }

private:
  [[nodiscard]] std::vector<LiveControlCommand> parse_line(std::string line);

  int descriptor_{};
  DisplayGeometry geometry_;
  std::string buffered_input_;
  bool closed_{};
};

} // namespace ilemu
