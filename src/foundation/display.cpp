#include "ilemu/display.hpp"

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
}

void DisplayState::replace_pixels(std::vector<std::uint32_t> pixels) {
  const auto expected = geometry_.pixel_count();
  if (pixels.size() != expected)
    return;
  std::lock_guard lock{mutex_};
  pixels_ = std::move(pixels);
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
    frame = DisplayFrame{geometry_.width, geometry_.height, sequence_,
                         visible_pixels(pixels_, powered_on_)};
  }
  presenter(frame);
}

void DisplayState::present() {
  Presenter presenter;
  DisplayFrame frame;
  {
    std::lock_guard lock{mutex_};
    ++sequence_;
    presenter = presenter_;
    if (!presenter)
      return;
    frame = DisplayFrame{geometry_.width, geometry_.height, sequence_,
                         visible_pixels(pixels_, powered_on_)};
  }
  presenter(frame);
}

DisplayFrame DisplayState::snapshot() const {
  std::lock_guard lock{mutex_};
  return DisplayFrame{geometry_.width, geometry_.height, sequence_,
                      visible_pixels(pixels_, powered_on_)};
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
