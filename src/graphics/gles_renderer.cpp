#include "ilegacysim/gles_renderer.hpp"

#include <memory>
#include <span>
#include <string_view>
#include <utility>

#include "ilegacysim/display.hpp"

#if defined(ILEGACYSIM_HAS_VULKAN)
#include "host/vulkan_gles_renderer.hpp"
#endif

namespace ilegacysim {
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
        return "iLegacySim GLES 1.1 software";
    }

    [[nodiscard]] bool accelerated() const override { return false; }
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

  private:
    std::unique_ptr<GlesRenderer> primary_;
    std::unique_ptr<GlesRenderer> fallback_;
};

} // namespace

std::shared_ptr<GlesRenderer> shared_gles_renderer() {
    static const auto renderer = []() -> std::shared_ptr<GlesRenderer> {
#if defined(ILEGACYSIM_HAS_VULKAN)
        if (auto accelerated = create_vulkan_gles_renderer()) {
            return std::make_shared<FallbackGlesRenderer>(
                std::move(accelerated),
                std::make_unique<SoftwareGlesRenderer>());
        }
#endif
        return std::make_shared<SoftwareGlesRenderer>();
    }();
    return renderer;
}

} // namespace ilegacysim
