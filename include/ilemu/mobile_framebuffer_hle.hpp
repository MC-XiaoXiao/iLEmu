#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>

namespace ilemu {

class DisplayState;
class PresentationTracker;
class SceneCoordinator;
struct KernelSharedState;
class UserlandHleCall;
class UserlandHleRegistry;
class SurfaceStore;

class MobileFramebufferHle {
public:
    MobileFramebufferHle(UserlandHleRegistry& registry,
                         std::shared_ptr<DisplayState> display,
                         std::shared_ptr<SurfaceStore> surfaces = {},
                         std::shared_ptr<PresentationTracker> presentations = {});

    void reset();
    void inherit_state(const MobileFramebufferHle& parent);
    void set_display(std::shared_ptr<DisplayState> display);
    void set_shared_state(std::shared_ptr<KernelSharedState> shared_state);
    void set_presentation_tracker(
        std::shared_ptr<PresentationTracker> presentations);
    void set_scene_coordinator(std::shared_ptr<SceneCoordinator> scenes);
    [[nodiscard]] bool has_active_layers() const;

private:
    void set_background_color(UserlandHleCall& call);
    void set_layer(UserlandHleCall& call);
    void submit_layers(UserlandHleCall& call);
    void record_presentation(UserlandHleCall& call);
    [[nodiscard]] bool display_write_allowed(UserlandHleCall& call) const;

    struct Rectangle {
        float x{};
        float y{};
        float width{};
        float height{};
    };
    struct LayerState {
        std::uint32_t surface_id{};
        Rectangle source;
        Rectangle destination;
        std::uint32_t flags{};
    };

    std::shared_ptr<DisplayState> display_;
    std::shared_ptr<SurfaceStore> surface_store_;
    std::shared_ptr<PresentationTracker> presentation_tracker_;
    std::shared_ptr<KernelSharedState> shared_state_;
    std::shared_ptr<SceneCoordinator> scene_coordinator_;
    std::map<std::uint32_t, LayerState> layers_;
    std::uint32_t next_swap_id_{1};
    std::uint32_t background_argb_{0xff000000U};
};

}  // namespace ilemu
