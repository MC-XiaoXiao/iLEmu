#include "ilemu/live_control.hpp"

#include <cerrno>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>

#include <poll.h>
#include <unistd.h>

namespace ilemu {
namespace {

constexpr std::size_t read_buffer_size = 4096;
constexpr std::size_t maximum_buffered_input = 64 * 1024;
constexpr std::int64_t default_drag_duration_ms = 300;
constexpr std::size_t default_drag_steps = 6;
constexpr std::int64_t maximum_drag_duration_ms = 5'000;
constexpr std::size_t maximum_drag_steps = 64;
constexpr std::int64_t default_tap_duration_ms = 80;
constexpr auto drag_release_delay = std::chrono::milliseconds{16};
// Reproduce the smallest trajectory that has been verified against the stock
// iPhone OS 1.0 lock slider: Down at the knob center, seven 30-pixel moves at
// 200 ms intervals, then Up after one final 200 ms hold.
constexpr float unlock_start_x_fraction = 0.15625F;
constexpr float unlock_y_fraction = 0.8958333333F;
constexpr float unlock_end_x_fraction = 0.8125F;
constexpr std::int64_t unlock_duration_ms = 1'400;
constexpr std::size_t unlock_steps = 7;
constexpr auto unlock_release_delay = std::chrono::milliseconds{200};
// A Home click is processed by SpringBoard before it requests panel power and
// tears down the dimming window. Do not begin the lock gesture in that window.
constexpr auto unlock_wake_settle_delay = std::chrono::milliseconds{1'000};

LiveControlCommand error_command(std::string message) {
  LiveControlCommand result;
  result.kind = LiveControlCommandKind::Error;
  result.message = std::move(message);
  return result;
}

LiveControlCommand simple_command(LiveControlCommandKind kind) {
  LiveControlCommand result;
  result.kind = kind;
  return result;
}

std::string trim(std::string value) {
  const auto first = value.find_first_not_of(" \t\r");
  if (first == std::string::npos)
    return {};
  const auto last = value.find_last_not_of(" \t\r");
  return value.substr(first, last - first + 1U);
}

bool valid_touch_position(float x, float y, DisplayGeometry geometry) {
  return std::isfinite(x) && std::isfinite(y) && x >= 0.0F && y >= 0.0F &&
         x < static_cast<float>(geometry.width) &&
         y < static_cast<float>(geometry.height);
}

LiveControlCommand make_drag_command(
    float start_x, float start_y, float end_x, float end_y,
    std::int64_t duration_ms, std::size_t steps, std::string gesture_name,
    std::chrono::milliseconds release_delay = drag_release_delay,
    std::chrono::milliseconds start_delay = std::chrono::milliseconds{0}) {
  LiveControlCommand command;
  command.kind = LiveControlCommandKind::Gesture;
  command.message = std::move(gesture_name);
  command.gesture.reserve(steps + 2U);
  command.gesture.push_back(LiveTouchEvent{
      start_delay, TouchInput{TouchPhase::Down, start_x, start_y}});
  const auto duration = std::chrono::milliseconds{duration_ms};
  for (std::size_t step = 1; step <= steps; ++step) {
    const auto fraction = static_cast<float>(step) / static_cast<float>(steps);
    command.gesture.push_back(LiveTouchEvent{
        start_delay + duration * step / steps,
        TouchInput{TouchPhase::Move, start_x + (end_x - start_x) * fraction,
                   start_y + (end_y - start_y) * fraction}});
  }
  command.gesture.push_back(
      LiveTouchEvent{start_delay + duration + release_delay,
                     TouchInput{TouchPhase::Up, end_x, end_y}});
  return command;
}

std::optional<TouchPhase> parse_phase(std::string_view value) {
  if (value == "down")
    return TouchPhase::Down;
  if (value == "move")
    return TouchPhase::Move;
  if (value == "up")
    return TouchPhase::Up;
  if (value == "cancel")
    return TouchPhase::Cancel;
  return std::nullopt;
}

} // namespace

LiveControl::LiveControl(int descriptor, DisplayGeometry geometry)
    : descriptor_{descriptor},
      geometry_{geometry.valid() ? geometry : default_display_geometry} {
  if (descriptor_ < 0)
    closed_ = true;
}

std::vector<LiveControlCommand> LiveControl::poll() {
  std::vector<LiveControlCommand> commands;
  if (closed_)
    return commands;

  pollfd descriptor{descriptor_, POLLIN | POLLHUP, 0};
  const auto ready = ::poll(&descriptor, 1, 0);
  if (ready < 0) {
    if (errno != EINTR) {
      closed_ = true;
      commands.push_back(error_command("stdin polling failed"));
    }
    return commands;
  }
  if (ready == 0)
    return commands;

  if ((descriptor.revents & POLLIN) != 0) {
    char bytes[read_buffer_size];
    const auto received = ::read(descriptor_, bytes, sizeof(bytes));
    if (received > 0) {
      buffered_input_.append(bytes, static_cast<std::size_t>(received));
    } else if (received == 0) {
      closed_ = true;
    } else if (errno != EINTR && errno != EAGAIN) {
      closed_ = true;
      commands.push_back(error_command("stdin read failed"));
    }
  }
  if ((descriptor.revents & POLLHUP) != 0 &&
      (descriptor.revents & POLLIN) == 0) {
    closed_ = true;
  }

  if (buffered_input_.size() > maximum_buffered_input) {
    buffered_input_.clear();
    commands.push_back(error_command("control input exceeded 64 KiB"));
    return commands;
  }

  std::size_t newline = 0;
  while ((newline = buffered_input_.find('\n')) != std::string::npos) {
    auto parsed = parse_line(buffered_input_.substr(0, newline));
    buffered_input_.erase(0, newline + 1U);
    commands.insert(commands.end(), std::make_move_iterator(parsed.begin()),
                    std::make_move_iterator(parsed.end()));
  }
  return commands;
}

std::vector<LiveControlCommand> LiveControl::parse_line(std::string line) {
  line = trim(std::move(line));
  if (line.empty() || line.front() == '#')
    return {};

  std::istringstream parser{line};
  std::string operation;
  parser >> operation;
  if (operation == "quit")
    return {simple_command(LiveControlCommandKind::Quit)};
  if (operation == "status")
    return {simple_command(LiveControlCommandKind::Status)};
  if (operation == "help")
    return {simple_command(LiveControlCommandKind::Help)};
  if (operation == "wake") {
    std::string trailing;
    if (parser >> trailing)
      return {error_command("wake does not accept arguments")};
    return {simple_command(LiveControlCommandKind::Wake)};
  }
  if (operation == "lock") {
    std::string trailing;
    if (parser >> trailing)
      return {error_command("lock does not accept arguments")};
    return {simple_command(LiveControlCommandKind::Lock)};
  }
  if (operation == "volume-up" || operation == "volume-down") {
    std::string trailing;
    if (parser >> trailing) {
      return {error_command(operation + " does not accept arguments")};
    }
    return {simple_command(operation == "volume-up"
                               ? LiveControlCommandKind::VolumeUp
                               : LiveControlCommandKind::VolumeDown)};
  }
  if (operation == "ringer") {
    std::string state;
    std::string trailing;
    if (!(parser >> state) || (parser >> trailing) ||
        (state != "ring" && state != "silent")) {
      return {error_command("ringer requires exactly ring or silent")};
    }
    return {simple_command(state == "ring"
                               ? LiveControlCommandKind::RingerRing
                               : LiveControlCommandKind::RingerSilent)};
  }
  if (operation == "snapshot") {
    std::string path;
    std::getline(parser, path);
    path = trim(std::move(path));
    if (path.empty())
      return {error_command("snapshot requires an output path")};
    LiveControlCommand command;
    command.kind = LiveControlCommandKind::Snapshot;
    command.path = std::move(path);
    return {std::move(command)};
  }
  if (operation == "snapshot-sequence") {
    std::string path_prefix;
    std::int64_t interval_ms = 0;
    std::size_t count = 0;
    if (!(parser >> path_prefix >> interval_ms >> count)) {
      return {error_command(
          "snapshot-sequence requires PATH-PREFIX INTERVAL-MS COUNT")};
    }
    std::string trailing;
    if (parser >> trailing) {
      return {error_command("snapshot-sequence accepts exactly 3 arguments")};
    }
    constexpr std::int64_t maximum_interval_ms = 60'000;
    constexpr std::size_t maximum_snapshot_count = 1'000;
    if (interval_ms < 0 || interval_ms > maximum_interval_ms) {
      return {error_command(
          "snapshot-sequence interval must be between 0 and 60000 ms")};
    }
    if (count == 0 || count > maximum_snapshot_count) {
      return {error_command(
          "snapshot-sequence count must be between 1 and 1000")};
    }
    LiveControlCommand command;
    command.kind = LiveControlCommandKind::SnapshotSequence;
    command.path = std::move(path_prefix);
    command.snapshot_interval = std::chrono::milliseconds{interval_ms};
    command.snapshot_count = count;
    return {std::move(command)};
  }
  if (operation == "unlock") {
    std::string trailing;
    if (parser >> trailing)
      return {error_command("unlock does not accept arguments")};
    const auto width = static_cast<float>(geometry_.width);
    const auto height = static_cast<float>(geometry_.height);
    auto command = make_drag_command(
        width * unlock_start_x_fraction, height * unlock_y_fraction,
        width * unlock_end_x_fraction, height * unlock_y_fraction,
        unlock_duration_ms, unlock_steps, "unlock", unlock_release_delay,
        unlock_wake_settle_delay);
    command.wake_display = true;
    return {std::move(command)};
  }
  if (operation == "drag") {
    float start_x = 0.0F;
    float start_y = 0.0F;
    float end_x = 0.0F;
    float end_y = 0.0F;
    std::int64_t duration_ms = default_drag_duration_ms;
    std::size_t steps = default_drag_steps;
    if (!(parser >> start_x >> start_y >> end_x >> end_y)) {
      return {error_command("drag requires x1 y1 x2 y2 [duration-ms] [steps]")};
    }
    parser >> std::ws;
    if (!parser.eof()) {
      if (!(parser >> duration_ms)) {
        return {error_command("drag duration must be an integer")};
      }
      parser >> std::ws;
      if (!parser.eof()) {
        if (!(parser >> steps)) {
          return {error_command("drag steps must be an integer")};
        }
        parser >> std::ws;
      }
    }
    if (!parser.eof() || !valid_touch_position(start_x, start_y, geometry_) ||
        !valid_touch_position(end_x, end_y, geometry_) || duration_ms <= 0 ||
        duration_ms > maximum_drag_duration_ms || steps == 0 ||
        steps > maximum_drag_steps ||
        duration_ms < static_cast<std::int64_t>(steps)) {
      return {error_command(
          "drag coordinates must be inside " +
          std::to_string(geometry_.width) + "x" +
          std::to_string(geometry_.height) + "; duration must be "
          "1..5000 ms and at least steps; steps must be 1..64")};
    }

    return {make_drag_command(start_x, start_y, end_x, end_y, duration_ms,
                              steps, "drag")};
  }

  std::string phase_name;
  if (operation == "touch") {
    parser >> phase_name;
  } else {
    phase_name = operation;
  }
  if (operation == "tap") {
    float x = 0.0F;
    float y = 0.0F;
    std::int64_t duration_ms = default_tap_duration_ms;
    if (!(parser >> x >> y)) {
      return {error_command("tap requires x y [hold-ms]")};
    }
    parser >> std::ws;
    if (!parser.eof()) {
      if (!(parser >> duration_ms))
        return {error_command("tap hold duration must be an integer")};
      parser >> std::ws;
    }
    if (!parser.eof() || !valid_touch_position(x, y, geometry_) ||
        duration_ms <= 0 ||
        duration_ms > maximum_drag_duration_ms) {
      return {error_command(
          "tap coordinates must be inside " +
          std::to_string(geometry_.width) + "x" +
          std::to_string(geometry_.height) +
          " and hold duration must be "
          "1..5000 ms")};
    }
    LiveControlCommand command;
    command.kind = LiveControlCommandKind::Gesture;
    command.message = "tap";
    command.gesture = {
        LiveTouchEvent{std::chrono::milliseconds{0},
                       TouchInput{TouchPhase::Down, x, y}},
        LiveTouchEvent{std::chrono::milliseconds{duration_ms},
                       TouchInput{TouchPhase::Up, x, y}},
    };
    return {std::move(command)};
  }

  const auto phase = parse_phase(phase_name);
  float x = 0.0F;
  float y = 0.0F;
  std::string trailing;
  if (!phase || !(parser >> x >> y) || (parser >> trailing) ||
      !valid_touch_position(x, y, geometry_)) {
    return {error_command(
        "expected touch <down|move|up|cancel> x y inside " +
        std::to_string(geometry_.width) + "x" +
        std::to_string(geometry_.height))};
  }
  LiveControlCommand command;
  command.kind = LiveControlCommandKind::Touch;
  command.touch = TouchInput{*phase, x, y};
  return {std::move(command)};
}

} // namespace ilemu
