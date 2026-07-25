#include "ilemu/gles_renderer.hpp"

#include <memory>
#include <mutex>
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

  private:
    std::unique_ptr<GlesRenderer> primary_;
    std::unique_ptr<GlesRenderer> fallback_;
};

struct SharedRendererState {
    std::mutex mutex;
    GlesBackend backend{GlesBackend::Auto};
    std::shared_ptr<GlesRenderer>* renderer{};
};

SharedRendererState& shared_renderer_state() {
    static SharedRendererState state;
    return state;
}

std::shared_ptr<GlesRenderer> create_renderer(GlesBackend backend) {
    if (backend == GlesBackend::Software) {
        return std::make_shared<SoftwareGlesRenderer>();
    }

    std::string failure;
#if defined(ILEMU_HAS_VULKAN)
    if (auto accelerated = create_vulkan_gles_renderer(&failure)) {
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

std::shared_ptr<GlesRenderer>& renderer_slot(GlesBackend backend) {
    // Register this holder's destructor only after create_renderer() returns.
    // Vulkan ICDs may register their own process-lifetime teardown during
    // device creation; the renderer must be destroyed before those callbacks.
    static std::shared_ptr<GlesRenderer> renderer = create_renderer(backend);
    return renderer;
}

} // namespace

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
        state.renderer = &renderer_slot(state.backend);
    if (!*state.renderer)
        *state.renderer = create_renderer(state.backend);
    return *state.renderer;
}

void shutdown_gles_renderer() {
    auto& state = shared_renderer_state();
    std::lock_guard lock{state.mutex};
    if (state.renderer != nullptr)
        state.renderer->reset();
}

} // namespace ilemu
