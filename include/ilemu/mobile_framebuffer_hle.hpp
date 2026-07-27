#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>

#include "ilemu/host_graphics.hpp"

namespace ilemu {

class DisplayState;
class GlesRenderer;
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
    [[nodiscard]] bool
    application_surface_allowed(std::uint32_t producer_process_id,
                                std::uint64_t publication_sequence) const;
    [[nodiscard]] bool submit_host_layers(std::uint32_t owner_process_id);
    void ensure_scanout_surface();

    struct Rectangle {
        float x{};
        float y{};
        float width{};
        float height{};

        bool operator==(const Rectangle&) const = default;
    };
    struct LayerState {
        std::uint32_t surface_id{};
        Rectangle source;
        Rectangle destination;
        std::uint32_t flags{};

        bool operator==(const LayerState&) const = default;
    };
    struct SubmittedLayer {
        LayerState state;
        HostSurfaceKey surface_key;
        std::uint64_t generation{};
    };

    std::shared_ptr<DisplayState> display_;
    std::shared_ptr<SurfaceStore> surface_store_;
    std::shared_ptr<PresentationTracker> presentation_tracker_;
    std::shared_ptr<KernelSharedState> shared_state_;
    std::shared_ptr<SceneCoordinator> scene_coordinator_;
    std::shared_ptr<GlesRenderer> host_graphics_;
    std::unique_ptr<CommandEncoder> command_encoder_;
    std::shared_ptr<HostSurface> scanout_surface_;
    std::map<std::uint32_t, LayerState> layers_;
    std::map<std::uint32_t, SubmittedLayer> submitted_layers_;
    std::uint32_t next_swap_id_{1};
    std::uint32_t background_argb_{0xff000000U};
    std::uint32_t submitted_background_argb_{0xff000000U};
    bool scanout_contents_valid_{};
};

}  // namespace ilemu
