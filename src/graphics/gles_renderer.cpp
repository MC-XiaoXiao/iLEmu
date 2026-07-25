#include "ilemu/gles_renderer.hpp"

#include <memory>
#include <mutex>
#include <filesystem>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include "ilemu/display.hpp"

#if defined(ILEMU_HAS_VULKAN)
#include "host/vulkan_gles_renderer.hpp"
#endif

namespace ilemu {
namespace {

class SoftwareGlesRenderer final : public GlesRenderer {
  public:
    bool draw(DisplayFrame& frame, GlesRenderTargetKey target,
              std::span<const GlesRasterVertex> vertices, std::uint32_t mode,
              const GlesRasterState& state) override {
        static_cast<void>(target);
        return GlesSoftwareRasterizer::draw(frame, vertices, mode, state);
    }

    bool synchronize(DisplayFrame& frame, GlesRenderTargetKey target) override {
        static_cast<void>(frame);
        static_cast<void>(target);
        return true;
    }

    bool flush(GlesRenderTargetKey target) override {
        static_cast<void>(target);
        return true;
    }

    bool finish(GlesRenderTargetKey target) override {
        static_cast<void>(target);
        return true;
    }

    void invalidate(GlesRenderTargetKey target) override {
        static_cast<void>(target);
    }

    void release(GlesRenderTargetKey target) override {
        static_cast<void>(target);
    }

    [[nodiscard]] std::string_view name() const override {
        return "iLEmu GLES 1.1 software";
    }

    [[nodiscard]] bool accelerated() const override { return false; }
    [[nodiscard]] bool software_fallback_allowed() const override {
        return false;
    }
    [[nodiscard]] PerfFallbackReason failure_reason() const override {
        return PerfFallbackReason::None;
    }
};

class FallbackGlesRenderer final : public GlesRenderer {
  public:
    FallbackGlesRenderer(std::unique_ptr<GlesRenderer> primary,
                         std::unique_ptr<GlesRenderer> fallback)
        : primary_{std::move(primary)}, fallback_{std::move(fallback)} {}

    bool draw(DisplayFrame& frame, GlesRenderTargetKey target,
              std::span<const GlesRasterVertex> vertices, std::uint32_t mode,
              const GlesRasterState& state) override {
        if (primary_->draw(frame, target, vertices, mode, state))
            return true;
        performance_counters().record_fallback(primary_->failure_reason());
        if (!primary_->synchronize(frame, target) ||
            !fallback_->draw(frame, target, vertices, mode, state)) {
            return false;
        }
        primary_->invalidate(target);
        return true;
    }

    bool synchronize(DisplayFrame& frame, GlesRenderTargetKey target) override {
        return primary_->synchronize(frame, target) &&
               fallback_->synchronize(frame, target);
    }

    bool flush(GlesRenderTargetKey target) override {
        return primary_->flush(target) && fallback_->flush(target);
    }

    bool finish(GlesRenderTargetKey target) override {
        return primary_->finish(target) && fallback_->finish(target);
    }

    void invalidate(GlesRenderTargetKey target) override {
        primary_->invalidate(target);
        fallback_->invalidate(target);
    }

    void release(GlesRenderTargetKey target) override {
        primary_->release(target);
        fallback_->release(target);
    }

    [[nodiscard]] std::string_view name() const override {
        return primary_->name();
    }

    [[nodiscard]] bool accelerated() const override {
        return primary_->accelerated();
    }
    [[nodiscard]] bool software_fallback_allowed() const override {
        return true;
    }
    [[nodiscard]] PerfFallbackReason failure_reason() const override {
        return primary_->failure_reason();
    }
    [[nodiscard]] HostNativeImage
    native_image(const HostSurface& surface) const override {
        return primary_->native_image(surface);
    }
    [[nodiscard]] bool
    present(const std::shared_ptr<HostSurface>& surface) override {
        return primary_->present(surface);
    }
    [[nodiscard]] bool native_presentation_available() const override {
        return primary_->native_presentation_available();
    }
    [[nodiscard]] std::unique_ptr<CommandEncoder>
    create_command_encoder() override {
        return primary_->create_command_encoder();
    }

  private:
    std::unique_ptr<GlesRenderer> primary_;
    std::unique_ptr<GlesRenderer> fallback_;
};

struct SharedRendererState {
    std::mutex mutex;
    GlesBackend backend{GlesBackend::Auto};
    std::filesystem::path pipeline_cache;
    VulkanPresenterConfiguration presenter;
    std::shared_ptr<GlesRenderer>* renderer{};
};

SharedRendererState& shared_renderer_state() {
    static SharedRendererState state;
    return state;
}

std::shared_ptr<GlesRenderer>
create_renderer(GlesBackend backend,
                const std::filesystem::path& pipeline_cache,
                const VulkanPresenterConfiguration& presenter) {
    if (backend == GlesBackend::Software) {
        return std::make_shared<SoftwareGlesRenderer>();
    }

    std::string failure;
#if defined(ILEMU_HAS_VULKAN)
    if (auto accelerated =
            create_vulkan_gles_renderer(
                pipeline_cache,
                presenter.create_surface ? &presenter : nullptr, &failure)) {
        if (backend == GlesBackend::Vulkan) {
            return std::shared_ptr<GlesRenderer>{std::move(accelerated)};
        }
        return std::make_shared<FallbackGlesRenderer>(
            std::move(accelerated),
            std::make_unique<SoftwareGlesRenderer>());
    }
    performance_counters().record_fallback(
        PerfFallbackReason::VulkanUnavailable);
#else
    failure = "Vulkan support was not built";
    performance_counters().record_fallback(
        PerfFallbackReason::VulkanUnavailable);
#endif

    if (backend == GlesBackend::Vulkan) {
        throw std::runtime_error{
            "forced Vulkan GLES backend unavailable: " + failure};
    }
    return std::make_shared<SoftwareGlesRenderer>();
}

std::shared_ptr<GlesRenderer>&
renderer_slot(GlesBackend backend,
              const std::filesystem::path& pipeline_cache,
              const VulkanPresenterConfiguration& presenter) {
    // Register this holder's destructor only after create_renderer() returns.
    // Vulkan ICDs may register their own process-lifetime teardown during
    // device creation; the renderer must be destroyed before those callbacks.
    static std::shared_ptr<GlesRenderer> renderer =
        create_renderer(backend, pipeline_cache, presenter);
    return renderer;
}

} // namespace

std::shared_ptr<HostSurface>
GlesRenderer::create_surface(HostSurfaceKey key,
                             HostSurfaceDescriptor descriptor,
                             std::span<const std::uint32_t> initial_pixels) {
    return make_host_surface(key, descriptor, initial_pixels);
}

std::unique_ptr<CommandEncoder>
GlesRenderer::create_command_encoder() {
    return make_cpu_command_encoder();
}

bool GlesRenderer::map_cpu(HostSurface& surface, bool read) {
    if (!read)
        return true;
    const auto generation = surface.gpu_generation();
    {
        auto mapping = surface.map_cpu(false);
        if (!synchronize(mapping.frame(), surface.key()))
            return false;
    }
    surface.mark_cpu_synchronized(generation);
    return true;
}

HostNativeImage
GlesRenderer::native_image(const HostSurface& surface) const {
    static_cast<void>(surface);
    return {};
}

bool GlesRenderer::present(const std::shared_ptr<HostSurface>& surface) {
    static_cast<void>(surface);
    return false;
}

bool GlesRenderer::native_presentation_available() const {
    return false;
}

void configure_gles_backend(GlesBackend backend) {
    auto& state = shared_renderer_state();
    std::lock_guard lock{state.mutex};
    if (state.renderer != nullptr && *state.renderer &&
        state.backend != backend) {
        throw std::logic_error{
            "GLES backend cannot change after renderer initialization"};
    }
    state.backend = backend;
}

void configure_gles_pipeline_cache(std::filesystem::path path) {
    auto& state = shared_renderer_state();
    std::lock_guard lock{state.mutex};
    if (state.renderer != nullptr && *state.renderer) {
        throw std::logic_error{
            "GLES pipeline cache path cannot change after initialization"};
    }
    state.pipeline_cache = std::move(path);
}

void configure_gles_vulkan_presenter(
    VulkanPresenterConfiguration configuration) {
    auto& state = shared_renderer_state();
    std::lock_guard lock{state.mutex};
    if (state.renderer != nullptr && *state.renderer) {
        throw std::logic_error{
            "Vulkan presenter cannot change after renderer initialization"};
    }
    state.presenter = std::move(configuration);
}

std::string_view gles_backend_name(GlesBackend backend) {
    switch (backend) {
    case GlesBackend::Auto: return "auto";
    case GlesBackend::Software: return "software";
    case GlesBackend::Vulkan: return "vulkan";
    }
    return "unknown";
}

std::shared_ptr<GlesRenderer> shared_gles_renderer() {
    auto& state = shared_renderer_state();
    std::lock_guard lock{state.mutex};
    if (state.renderer == nullptr)
        state.renderer = &renderer_slot(
            state.backend, state.pipeline_cache, state.presenter);
    if (!*state.renderer)
        *state.renderer = create_renderer(
            state.backend, state.pipeline_cache, state.presenter);
    return *state.renderer;
}

void release_gles_render_target(GlesRenderTargetKey target) {
    auto& state = shared_renderer_state();
    std::lock_guard lock{state.mutex};
    if (state.renderer != nullptr && *state.renderer)
        (*state.renderer)->release(target);
}

void shutdown_gles_renderer() {
    auto& state = shared_renderer_state();
    std::lock_guard lock{state.mutex};
    if (state.renderer != nullptr)
        state.renderer->reset();
}

} // namespace ilemu
