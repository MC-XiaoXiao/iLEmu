#include "ilemu/display.hpp"
#include "ilemu/performance.hpp"

#include <algorithm>
#include <utility>

namespace ilemu {
namespace {

std::vector<std::uint32_t>
visible_pixels(const std::vector<std::uint32_t> &scanout, bool powered_on) {
  if (powered_on)
    return scanout;
  return std::vector<std::uint32_t>(scanout.size(), 0xff000000U);
}

} // namespace

DisplayState::DisplayState() : DisplayState(default_display_geometry) {}

DisplayState::DisplayState(DisplayGeometry geometry)
    : geometry_{geometry.valid() ? geometry : default_display_geometry},
      pixels_(geometry_.pixel_count(), 0xff000000U) {}

void DisplayState::set_presenter(Presenter presenter) {
  std::lock_guard lock{mutex_};
  presenter_ = std::move(presenter);
}

void DisplayState::clear(std::uint32_t argb) {
  std::lock_guard lock{mutex_};
  std::fill(pixels_.begin(), pixels_.end(), argb);
  host_surface_.reset();
  surface_reader_ = {};
  content_owner_process_id_ = 0;
}

void DisplayState::replace_pixels(std::vector<std::uint32_t> pixels,
                                  std::uint32_t owner_process_id) {
  const auto expected = geometry_.pixel_count();
  if (pixels.size() != expected)
    return;
  std::lock_guard lock{mutex_};
  pixels_ = std::move(pixels);
  host_surface_.reset();
  surface_reader_ = {};
  if (owner_process_id != 0)
    content_owner_process_id_ = owner_process_id;
}

void DisplayState::replace_surface(
    std::shared_ptr<HostSurface> surface,
    std::function<std::vector<std::uint32_t>()> read_pixels,
    std::uint32_t owner_process_id) {
  if (!surface || !read_pixels)
    return;
  std::lock_guard lock{mutex_};
  host_surface_ = std::move(surface);
  surface_reader_ = std::move(read_pixels);
  if (owner_process_id != 0)
    content_owner_process_id_ = owner_process_id;
}

void DisplayState::set_powered_on(bool powered_on) {
  Presenter presenter;
  DisplayFrame frame;
  {
    std::lock_guard lock{mutex_};
    if (powered_on_ == powered_on)
      return;
    powered_on_ = powered_on;
    ++sequence_;
    presenter = presenter_;
    if (!presenter)
      return;
    if (powered_on_ && host_surface_) {
      frame = DisplayFrame{geometry_.width, geometry_.height, sequence_, {},
                           host_surface_, surface_reader_,
                           content_owner_process_id_};
    } else {
      frame = DisplayFrame{geometry_.width, geometry_.height, sequence_,
                           visible_pixels(pixels_, powered_on_), {}, {},
                           content_owner_process_id_};
    }
  }
  const PerformanceLatencyScope latency{PerfLatencyKind::DisplayPresent};
  auto &performance = performance_counters();
  if (performance.enabled()) {
    frame.submitted_at = std::chrono::steady_clock::now();
    performance.record_display_submission(
        frame.sequence, frame.owner_process_id, frame.submitted_at);
  }
  presenter(frame);
}

void DisplayState::present(std::uint32_t owner_process_id) {
  Presenter presenter;
  DisplayFrame frame;
  {
    std::lock_guard lock{mutex_};
    ++sequence_;
    if (owner_process_id != 0)
      content_owner_process_id_ = owner_process_id;
    presenter = presenter_;
    if (!presenter)
      return;
    if (powered_on_ && host_surface_) {
      frame = DisplayFrame{geometry_.width, geometry_.height, sequence_, {},
                           host_surface_, surface_reader_,
                           content_owner_process_id_};
    } else {
      frame = DisplayFrame{geometry_.width, geometry_.height, sequence_,
                           visible_pixels(pixels_, powered_on_), {}, {},
                           content_owner_process_id_};
    }
  }
  const PerformanceLatencyScope latency{PerfLatencyKind::DisplayPresent};
  auto &performance = performance_counters();
  if (performance.enabled()) {
    frame.submitted_at = std::chrono::steady_clock::now();
    performance.record_display_submission(
        frame.sequence, frame.owner_process_id, frame.submitted_at);
  }
  presenter(frame);
}

bool DisplayState::clear_if_owner(std::uint32_t owner_process_id) {
  if (owner_process_id == 0)
    return false;
  Presenter presenter;
  DisplayFrame frame;
  {
    std::lock_guard lock{mutex_};
    if (content_owner_process_id_ != owner_process_id)
      return false;
    std::fill(pixels_.begin(), pixels_.end(), 0xff000000U);
    host_surface_.reset();
    surface_reader_ = {};
    content_owner_process_id_ = 0;
    ++sequence_;
    presenter = presenter_;
    if (!presenter)
      return true;
    frame = DisplayFrame{geometry_.width, geometry_.height, sequence_,
                         pixels_};
  }
  const PerformanceLatencyScope latency{PerfLatencyKind::DisplayPresent};
  auto &performance = performance_counters();
  if (performance.enabled()) {
    frame.submitted_at = std::chrono::steady_clock::now();
    performance.record_display_submission(
        frame.sequence, frame.owner_process_id, frame.submitted_at);
  }
  presenter(frame);
  return true;
}

DisplayFrame DisplayState::snapshot() const {
  std::function<std::vector<std::uint32_t>()> reader;
  std::shared_ptr<HostSurface> surface;
  std::vector<std::uint32_t> pixels;
  std::uint64_t sequence{};
  bool powered_on{};
  std::uint32_t owner_process_id{};
  {
    std::lock_guard lock{mutex_};
    reader = surface_reader_;
    surface = host_surface_;
    pixels = pixels_;
    sequence = sequence_;
    powered_on = powered_on_;
    owner_process_id = content_owner_process_id_;
  }
  if (!powered_on) {
    pixels.assign(geometry_.pixel_count(), 0xff000000U);
  } else if (surface && reader) {
    auto materialized = reader();
    if (materialized.size() == geometry_.pixel_count())
      pixels = std::move(materialized);
  }
  return DisplayFrame{geometry_.width, geometry_.height, sequence,
                      std::move(pixels), std::move(surface),
                      std::move(reader), owner_process_id};
}

std::uint64_t DisplayState::presented_frames() const {
  std::lock_guard lock{mutex_};
  return sequence_;
}

bool DisplayState::powered_on() const {
  std::lock_guard lock{mutex_};
  return powered_on_;
}

} // namespace ilemu
