#include "ilemu/host_graphics.hpp"

#include <algorithm>
#include <cstddef>
#include <limits>
#include <utility>
#include <vector>

namespace ilemu {
namespace {

HostRectangle union_rectangle(HostRectangle left, HostRectangle right) {
    const auto x = std::min(left.x, right.x);
    const auto y = std::min(left.y, right.y);
    const auto right_edge = std::max(
        static_cast<std::int64_t>(left.x) + left.width,
        static_cast<std::int64_t>(right.x) + right.width);
    const auto bottom_edge = std::max(
        static_cast<std::int64_t>(left.y) + left.height,
        static_cast<std::int64_t>(right.y) + right.height);
    return HostRectangle{
        x, y, static_cast<std::uint32_t>(right_edge - x),
        static_cast<std::uint32_t>(bottom_edge - y)};
}

std::uint32_t source_over(std::uint32_t source, std::uint32_t destination,
                          std::uint8_t global_alpha, bool premultiplied) {
    const auto scale = [](std::uint32_t value, std::uint32_t factor) {
        return (value * factor + 127U) / 255U;
    };
    const auto source_alpha =
        scale(source >> 24U, static_cast<std::uint32_t>(global_alpha));
    const auto inverse_alpha = 255U - source_alpha;
    std::uint32_t result{};
    for (std::uint32_t shift = 0; shift < 24U; shift += 8U) {
        const auto source_channel =
            scale((source >> shift) & 0xffU,
                  premultiplied ? global_alpha : source_alpha);
        const auto destination_channel =
            scale((destination >> shift) & 0xffU, inverse_alpha);
        result |= std::min(255U, source_channel + destination_channel)
                  << shift;
    }
    result |=
        std::min(255U, source_alpha +
                           scale(destination >> 24U, inverse_alpha))
        << 24U;
    return result;
}

std::uint32_t constant_alpha_crossfade(
    std::uint32_t source, std::uint32_t destination,
    std::uint8_t global_alpha) {
    const auto scale = [](std::uint32_t value, std::uint32_t factor) {
        return (value * factor + 127U) / 255U;
    };
    const auto alpha = static_cast<std::uint32_t>(global_alpha);
    const auto inverse_alpha = 255U - alpha;
    std::uint32_t result{};
    for (std::uint32_t shift = 0; shift < 32U; shift += 8U) {
        const auto source_channel =
            scale((source >> shift) & 0xffU, alpha);
        const auto destination_channel =
            scale((destination >> shift) & 0xffU, inverse_alpha);
        result |= std::min(255U, source_channel + destination_channel)
                  << shift;
    }
    return result;
}

class CpuCommandEncoder final : public CommandEncoder {
  public:
    bool fill(const std::shared_ptr<HostSurface>& destination,
              HostRectangle rectangle, std::uint32_t argb,
              HostCompositeMode mode,
              std::uint8_t global_alpha) override {
        if (!destination)
            return false;
        auto mapping = destination->map_cpu(
            true, PerfCpuMapReason::SoftwareFallback);
        auto& frame = mapping.frame();
        if (rectangle.x < 0 || rectangle.y < 0 ||
            rectangle.width > frame.width ||
            rectangle.height > frame.height ||
            static_cast<std::uint32_t>(rectangle.x) >
                frame.width - rectangle.width ||
            static_cast<std::uint32_t>(rectangle.y) >
                frame.height - rectangle.height) {
            return false;
        }
        for (std::uint32_t y = 0; y < rectangle.height; ++y) {
            const auto row =
                static_cast<std::size_t>(
                    static_cast<std::uint32_t>(rectangle.y) + y) *
                    frame.width +
                static_cast<std::uint32_t>(rectangle.x);
            for (std::uint32_t x = 0; x < rectangle.width; ++x) {
                auto& pixel = frame.pixels[row + x];
                if (mode == HostCompositeMode::Copy) {
                    pixel = argb;
                } else if (
                    mode == HostCompositeMode::ConstantAlphaCrossfade) {
                    pixel = constant_alpha_crossfade(
                        argb, pixel, global_alpha);
                } else {
                    pixel = source_over(
                        argb, pixel, global_alpha,
                        mode ==
                            HostCompositeMode::PremultipliedSourceOver);
                }
            }
        }
        performance_counters().record_host_fill();
        return true;
    }

    bool copy(const std::shared_ptr<HostSurface>& source,
              const std::shared_ptr<HostSurface>& destination,
              HostRectangle source_rectangle,
              HostRectangle destination_rectangle, HostCompositeMode mode,
              std::uint8_t global_alpha, HostFilter filter,
              HostRotation rotation) override {
        if (!source || !destination || source_rectangle.width == 0 ||
            source_rectangle.height == 0 ||
            destination_rectangle.width == 0 ||
            destination_rectangle.height == 0 ||
            filter == HostFilter::Linear) {
            return false;
        }
        DisplayFrame source_frame;
        {
            auto source_mapping = source->map_cpu(
                false, PerfCpuMapReason::SoftwareFallback);
            source_frame = source_mapping.frame();
        }
        auto destination_mapping = destination->map_cpu(
            true, PerfCpuMapReason::SoftwareFallback);
        auto& destination_frame = destination_mapping.frame();
        const auto valid = [](const DisplayFrame& frame,
                              HostRectangle rectangle) {
            return rectangle.x >= 0 && rectangle.y >= 0 &&
                   rectangle.width <= frame.width &&
                   rectangle.height <= frame.height &&
                   static_cast<std::uint32_t>(rectangle.x) <=
                       frame.width - rectangle.width &&
                   static_cast<std::uint32_t>(rectangle.y) <=
                       frame.height - rectangle.height;
        };
        if (!valid(source_frame, source_rectangle) ||
            !valid(destination_frame, destination_rectangle)) {
            return false;
        }
        std::vector<std::uint32_t> sampled(
            static_cast<std::size_t>(destination_rectangle.width) *
            destination_rectangle.height);
        for (std::uint32_t y = 0; y < destination_rectangle.height; ++y) {
            for (std::uint32_t x = 0; x < destination_rectangle.width; ++x) {
                std::uint32_t local_x{};
                std::uint32_t local_y{};
                if (rotation == HostRotation::Clockwise90) {
                    local_x = static_cast<std::uint32_t>(
                        static_cast<std::uint64_t>(y) *
                        source_rectangle.width /
                        destination_rectangle.height);
                    local_y = source_rectangle.height - 1U -
                              static_cast<std::uint32_t>(
                                  static_cast<std::uint64_t>(x) *
                                  source_rectangle.height /
                                  destination_rectangle.width);
                } else if (rotation == HostRotation::Rotate180) {
                    local_x = source_rectangle.width - 1U -
                              static_cast<std::uint32_t>(
                                  static_cast<std::uint64_t>(x) *
                                  source_rectangle.width /
                                  destination_rectangle.width);
                    local_y = source_rectangle.height - 1U -
                              static_cast<std::uint32_t>(
                                  static_cast<std::uint64_t>(y) *
                                  source_rectangle.height /
                                  destination_rectangle.height);
                } else if (rotation == HostRotation::Clockwise270) {
                    local_x = source_rectangle.width - 1U -
                              static_cast<std::uint32_t>(
                                  static_cast<std::uint64_t>(y) *
                                  source_rectangle.width /
                                  destination_rectangle.height);
                    local_y = static_cast<std::uint32_t>(
                        static_cast<std::uint64_t>(x) *
                        source_rectangle.height /
                        destination_rectangle.width);
                } else {
                    local_x = static_cast<std::uint32_t>(
                        static_cast<std::uint64_t>(x) *
                        source_rectangle.width /
                        destination_rectangle.width);
                    local_y = static_cast<std::uint32_t>(
                        static_cast<std::uint64_t>(y) *
                        source_rectangle.height /
                        destination_rectangle.height);
                }
                const auto source_x =
                    static_cast<std::uint32_t>(source_rectangle.x) + local_x;
                const auto source_y =
                    static_cast<std::uint32_t>(source_rectangle.y) + local_y;
                sampled[static_cast<std::size_t>(y) *
                            destination_rectangle.width +
                        x] =
                    source_frame.pixels[
                        static_cast<std::size_t>(source_y) *
                            source_frame.width +
                        source_x];
            }
        }
        for (std::uint32_t y = 0; y < destination_rectangle.height; ++y) {
            for (std::uint32_t x = 0; x < destination_rectangle.width; ++x) {
                const auto source_pixel =
                    sampled[static_cast<std::size_t>(y) *
                                destination_rectangle.width +
                            x];
                auto& destination_pixel =
                    destination_frame.pixels[
                        static_cast<std::size_t>(
                            static_cast<std::uint32_t>(
                                destination_rectangle.y) +
                            y) *
                            destination_frame.width +
                        static_cast<std::uint32_t>(destination_rectangle.x) +
                        x];
                if (mode == HostCompositeMode::Copy) {
                    destination_pixel = source_pixel;
                } else if (
                    mode == HostCompositeMode::ConstantAlphaCrossfade) {
                    destination_pixel = constant_alpha_crossfade(
                        source_pixel, destination_pixel, global_alpha);
                } else {
                    destination_pixel = source_over(
                        source_pixel, destination_pixel, global_alpha,
                        mode ==
                            HostCompositeMode::PremultipliedSourceOver);
                }
            }
        }
        performance_counters().record_host_copy();
        return true;
    }

    bool submit() override { return true; }
    bool finish() override { return true; }
};

} // namespace

HostSurface::CpuMapping::CpuMapping(HostSurface& surface, bool write)
    : surface_{&surface}, lock_{surface.mutex_}, write_{write} {}

HostSurface::CpuMapping::CpuMapping(CpuMapping&& other) noexcept
    : surface_{std::exchange(other.surface_, nullptr)},
      lock_{std::move(other.lock_)},
      write_{std::exchange(other.write_, false)} {}

HostSurface::CpuMapping::~CpuMapping() {
    if (surface_ && write_)
        surface_->mark_cpu_write_locked();
}

HostSurface::HostSurface(HostSurfaceKey key,
                         HostSurfaceDescriptor descriptor,
                         std::span<const std::uint32_t> initial_pixels)
    : key_{key},
      descriptor_{descriptor},
      cpu_frame_{descriptor.width, descriptor.height, 0,
                 std::vector<std::uint32_t>(
                     static_cast<std::size_t>(descriptor.width) *
                         descriptor.height,
                     0)},
      cpu_damage_{HostRectangle{0, 0, descriptor.width, descriptor.height}} {
    if (initial_pixels.size() == cpu_frame_.pixels.size())
        std::copy(initial_pixels.begin(), initial_pixels.end(),
                  cpu_frame_.pixels.begin());
}

std::uint64_t HostSurface::cpu_generation() const {
    std::lock_guard lock{mutex_};
    return cpu_generation_;
}

std::uint64_t HostSurface::gpu_generation() const {
    std::lock_guard lock{mutex_};
    return gpu_generation_;
}

HostSurface::CpuMapping HostSurface::map_cpu(
    bool write, PerfCpuMapReason reason) {
    performance_counters().record_cpu_map(write, reason);
    return CpuMapping{*this, write};
}

void HostSurface::replace_cpu(std::span<const std::uint32_t> pixels) {
    std::lock_guard lock{mutex_};
    if (pixels.size() != cpu_frame_.pixels.size())
        return;
    std::copy(pixels.begin(), pixels.end(), cpu_frame_.pixels.begin());
    mark_cpu_write_locked();
}

void HostSurface::replace_cpu_region(
    HostRectangle rectangle, std::span<const std::uint32_t> pixels) {
    std::lock_guard lock{mutex_};
    if (rectangle.x < 0 || rectangle.y < 0 || rectangle.width == 0 ||
        rectangle.height == 0 || rectangle.width > descriptor_.width ||
        rectangle.height > descriptor_.height ||
        static_cast<std::uint32_t>(rectangle.x) >
            descriptor_.width - rectangle.width ||
        static_cast<std::uint32_t>(rectangle.y) >
            descriptor_.height - rectangle.height ||
        pixels.size() !=
            static_cast<std::size_t>(rectangle.width) * rectangle.height) {
        return;
    }
    for (std::uint32_t row = 0; row < rectangle.height; ++row) {
        std::copy_n(
            pixels.begin() + static_cast<std::size_t>(row) * rectangle.width,
            rectangle.width,
            cpu_frame_.pixels.begin() +
                static_cast<std::size_t>(
                    static_cast<std::uint32_t>(rectangle.y) + row) *
                    descriptor_.width +
                static_cast<std::uint32_t>(rectangle.x));
    }
    cpu_generation_ = ++next_generation_;
    cpu_damage_ = cpu_damage_ ? union_rectangle(*cpu_damage_, rectangle)
                              : std::optional<HostRectangle>{rectangle};
}

std::optional<HostRectangle> HostSurface::cpu_damage() const {
    std::lock_guard lock{mutex_};
    return cpu_damage_;
}

std::uint64_t HostSurface::mark_gpu_write() {
    std::lock_guard lock{mutex_};
    gpu_generation_ = ++next_generation_;
    return gpu_generation_;
}

void HostSurface::mark_cpu_synchronized(std::uint64_t gpu_generation) {
    std::lock_guard lock{mutex_};
    cpu_generation_ = std::max(cpu_generation_, gpu_generation);
    next_generation_ = std::max(next_generation_, cpu_generation_);
    if (gpu_generation >= cpu_generation_)
        cpu_damage_.reset();
}

void HostSurface::mark_gpu_synchronized(std::uint64_t cpu_generation) {
    std::lock_guard lock{mutex_};
    gpu_generation_ = std::max(gpu_generation_, cpu_generation);
    next_generation_ = std::max(next_generation_, gpu_generation_);
    if (cpu_generation >= cpu_generation_)
        cpu_damage_.reset();
}

void HostSurface::mark_cpu_write_locked() {
    cpu_generation_ = ++next_generation_;
    cpu_damage_ =
        HostRectangle{0, 0, descriptor_.width, descriptor_.height};
}

std::shared_ptr<HostSurface>
make_host_surface(HostSurfaceKey key, HostSurfaceDescriptor descriptor,
                  std::span<const std::uint32_t> initial_pixels) {
    return std::make_shared<HostSurface>(key, descriptor, initial_pixels);
}

std::unique_ptr<CommandEncoder> make_cpu_command_encoder() {
    return std::make_unique<CpuCommandEncoder>();
}

} // namespace ilemu
