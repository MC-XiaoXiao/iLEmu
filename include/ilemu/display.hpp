#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <utility>
#include <vector>

#include "ilemu/display_geometry.hpp"

namespace ilemu {

class HostSurface;

struct DisplayFrame {
  DisplayFrame() = default;
  DisplayFrame(
      std::uint32_t frame_width, std::uint32_t frame_height,
      std::uint64_t frame_sequence, std::vector<std::uint32_t> frame_pixels,
      std::shared_ptr<HostSurface> frame_surface = {},
      std::function<std::vector<std::uint32_t>()> frame_reader = {})
      : width{frame_width}, height{frame_height}, sequence{frame_sequence},
        pixels{std::move(frame_pixels)},
        host_surface{std::move(frame_surface)},
        read_pixels{std::move(frame_reader)} {}

  std::uint32_t width{};
  std::uint32_t height{};
  std::uint64_t sequence{};
  // Host-endian 0xAARRGGBB pixels. Backends perform any required upload
  // format conversion without exposing it to the guest graphics HLE.
  std::vector<std::uint32_t> pixels;
  // A hardware presenter consumes the native surface directly. CPU sinks
  // invoke read_pixels only at an explicit screenshot/software boundary.
  std::shared_ptr<HostSurface> host_surface;
  std::function<std::vector<std::uint32_t>()> read_pixels;
};

class DisplayState {
public:
  using Presenter = std::function<void(const DisplayFrame &)>;

  DisplayState();
  explicit DisplayState(DisplayGeometry geometry);

  void set_presenter(Presenter presenter);
  void clear(std::uint32_t argb);
  void replace_pixels(std::vector<std::uint32_t> pixels);
  void replace_surface(
      std::shared_ptr<HostSurface> surface,
      std::function<std::vector<std::uint32_t>()> read_pixels);
  // The framebuffer keeps its last scanout contents while the LCD is off.
  // Presenters and snapshots expose a black panel until power is restored.
  void set_powered_on(bool powered_on);
  void present();

  [[nodiscard]] DisplayFrame snapshot() const;
  [[nodiscard]] std::uint64_t presented_frames() const;
  [[nodiscard]] bool powered_on() const;
  [[nodiscard]] DisplayGeometry geometry() const { return geometry_; }
  [[nodiscard]] std::uint32_t width() const { return geometry_.width; }
  [[nodiscard]] std::uint32_t height() const { return geometry_.height; }

private:
  DisplayGeometry geometry_;
  mutable std::mutex mutex_;
  std::vector<std::uint32_t> pixels_;
  std::shared_ptr<HostSurface> host_surface_;
  std::function<std::vector<std::uint32_t>()> surface_reader_;
  Presenter presenter_;
  std::uint64_t sequence_{};
  bool powered_on_{true};
};

} // namespace ilemu
