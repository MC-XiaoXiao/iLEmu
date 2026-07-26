#pragma once

#include <compare>
#include <cstdint>
#include <memory>
#include <mutex>
#include <span>

#include "ilemu/display.hpp"
#include "ilemu/performance.hpp"

namespace ilemu {

struct HostSurfaceKey {
    std::uint64_t owner{};
    std::uint64_t surface{};

    auto operator<=>(const HostSurfaceKey&) const = default;
};

struct HostSurfaceDescriptor {
    std::uint32_t width{};
    std::uint32_t height{};
    std::uint32_t bytes_per_row{};
    std::uint32_t pixel_format{};
    PerfSurfaceKind kind{PerfSurfaceKind::Unknown};
};

struct HostRectangle {
    std::int32_t x{};
    std::int32_t y{};
    std::uint32_t width{};
    std::uint32_t height{};
};

enum class HostCompositeMode : std::uint8_t {
    Copy,
    SourceOver,
    PremultipliedSourceOver,
};

enum class HostFilter : std::uint8_t {
    Nearest,
    Linear,
};

// Cross-frontend surface identity. The CPU and GPU generations describe which
// representation is authoritative; a backend owns the native image for key().
class HostSurface {
  public:
    class CpuMapping {
      public:
        CpuMapping(CpuMapping&& other) noexcept;
        CpuMapping& operator=(CpuMapping&&) = delete;
        ~CpuMapping();

        [[nodiscard]] DisplayFrame& frame() { return surface_->cpu_frame_; }
        [[nodiscard]] const DisplayFrame& frame() const {
            return surface_->cpu_frame_;
        }

      private:
        friend class HostSurface;
        CpuMapping(HostSurface& surface, bool write);

        HostSurface* surface_{};
        std::unique_lock<std::mutex> lock_;
        bool write_{};
    };

    HostSurface(HostSurfaceKey key, HostSurfaceDescriptor descriptor,
                std::span<const std::uint32_t> initial_pixels = {});

    [[nodiscard]] HostSurfaceKey key() const { return key_; }
    [[nodiscard]] HostSurfaceDescriptor descriptor() const {
        return descriptor_;
    }
    [[nodiscard]] std::uint64_t cpu_generation() const;
    [[nodiscard]] std::uint64_t gpu_generation() const;
    [[nodiscard]] CpuMapping
    map_cpu(bool write,
            PerfCpuMapReason reason = PerfCpuMapReason::Internal);
    void replace_cpu(std::span<const std::uint32_t> pixels);
    // Called after queueing a native image write and after a completed
    // GPU-to-CPU transfer, respectively.
    [[nodiscard]] std::uint64_t mark_gpu_write();
    void mark_cpu_synchronized(std::uint64_t gpu_generation);
    void mark_gpu_synchronized(std::uint64_t cpu_generation);

  private:
    void mark_cpu_write_locked();

    HostSurfaceKey key_;
    HostSurfaceDescriptor descriptor_;
    mutable std::mutex mutex_;
    DisplayFrame cpu_frame_;
    std::uint64_t next_generation_{1};
    std::uint64_t cpu_generation_{1};
    std::uint64_t gpu_generation_{};
};

class CommandEncoder {
  public:
    virtual ~CommandEncoder() = default;
    [[nodiscard]] virtual bool
    fill(const std::shared_ptr<HostSurface>& destination,
         HostRectangle rectangle, std::uint32_t argb,
         HostCompositeMode mode = HostCompositeMode::Copy,
         std::uint8_t global_alpha = 0xffU) = 0;
    [[nodiscard]] virtual bool
    copy(const std::shared_ptr<HostSurface>& source,
         const std::shared_ptr<HostSurface>& destination,
         HostRectangle source_rectangle, HostRectangle destination_rectangle,
         HostCompositeMode mode = HostCompositeMode::Copy,
         std::uint8_t global_alpha = 0xffU,
         HostFilter filter = HostFilter::Nearest) = 0;
    // Submit never implies completion. finish() is the explicit wait point.
    [[nodiscard]] virtual bool submit() = 0;
    [[nodiscard]] virtual bool finish() = 0;
};

struct HostNativeImage {
    enum class Api : std::uint8_t {
        None,
        Vulkan,
    };

    Api api{Api::None};
    std::uintptr_t device{};
    std::uintptr_t image{};
    std::uint32_t layout{};
    HostSurfaceDescriptor descriptor;
    std::uint64_t generation{};
};

class HostGraphicsDevice {
  public:
    virtual ~HostGraphicsDevice() = default;
    [[nodiscard]] virtual std::shared_ptr<HostSurface>
    create_surface(HostSurfaceKey key, HostSurfaceDescriptor descriptor,
                   std::span<const std::uint32_t> initial_pixels = {}) = 0;
    [[nodiscard]] virtual std::unique_ptr<CommandEncoder>
    create_command_encoder() = 0;
    [[nodiscard]] virtual bool
    map_cpu(HostSurface& surface, bool read,
            PerfCpuMapReason reason = PerfCpuMapReason::GpuReadback) = 0;
    [[nodiscard]] virtual HostNativeImage
    native_image(const HostSurface& surface) const = 0;
    [[nodiscard]] virtual bool
    present(const std::shared_ptr<HostSurface>& surface) = 0;
    [[nodiscard]] virtual bool native_presentation_available() const = 0;
};

[[nodiscard]] std::shared_ptr<HostSurface>
make_host_surface(HostSurfaceKey key, HostSurfaceDescriptor descriptor,
                  std::span<const std::uint32_t> initial_pixels = {});
[[nodiscard]] std::unique_ptr<CommandEncoder>
make_cpu_command_encoder();

} // namespace ilemu
