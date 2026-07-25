#include "ilemu/mbx2d_hle.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "ilemu/display.hpp"
#include "ilemu/userland_hle.hpp"

namespace ilemu {

void Mbx2dHle::initialize_destination(
    UserlandHleCall& call, RenderState& state) {
    const auto destination = resolve(state.destination);
    if (!destination || destination->framebuffer || !display_ ||
        destination->width != display_->width() ||
        destination->height != display_->height()) {
        return;
    }
    const auto surface = state.destination->surface;
    if (initialized_destinations_.contains(surface)) return;

    // CoreGraphics can rasterize static LayerKit content into a newly-created
    // CoreSurface before MBX2D binds it as the render destination.  Keep that
    // CPU-rendered backing store; replacing it with the previous scanout would
    // erase layers such as the lock-screen bottom bar before the first GPU
    // dirty update arrives.
    const auto existing = read_region(*destination, 0, 0, destination->width,
                                      destination->height, call);
    if (existing && std::any_of(existing->begin(), existing->end(),
                                [](std::uint32_t pixel) {
                                    return pixel != 0;
                                })) {
        initialized_destinations_.insert(surface);
        destination_frame_sequences_[surface] =
            display_ ? display_->presented_frames() : 0;
        return;
    }

    auto initial = display_ && display_->presented_frames() != 0
                       ? display_->snapshot().pixels
                       : std::vector<std::uint32_t>{};
    const auto pixel_count = static_cast<std::size_t>(destination->width) *
                             destination->height;
    if (initial.size() != pixel_count) {
        initial.assign(pixel_count, 0xff000000U);
    }
    if (write_region(*destination, 0, 0, destination->width,
                     destination->height, initial, call)) {
        initialized_destinations_.insert(surface);
        destination_frame_sequences_[surface] =
            display_ ? display_->presented_frames() : 0;
    }
}

void Mbx2dHle::prepare_destination_for_frame(
    UserlandHleCall& call, RenderState& state, DamageRegion damage,
    std::uint32_t source_surface) {
    initialize_destination(call, state);
    const auto destination = resolve(state.destination);
    if (!destination || destination->framebuffer || !display_ ||
        destination->width != display_->width() ||
        destination->height != display_->height()) {
        return;
    }
    const auto surface = state.destination->surface;
    const auto sequence = display_ ? display_->presented_frames() : 0;
    const auto prepared = destination_frame_sequences_.find(surface);
    if (prepared != destination_frame_sequences_.end() &&
        prepared->second == sequence) {
        if (const auto current = destination_scene_damage_.find(surface);
            current != destination_scene_damage_.end()) {
            current->second.left =
                std::min(current->second.left, damage.left);
            current->second.top = std::min(current->second.top, damage.top);
            current->second.right =
                std::max(current->second.right, damage.right);
            current->second.bottom =
                std::max(current->second.bottom, damage.bottom);
        } else {
            destination_scene_damage_[surface] = damage;
        }
        return;
    }
    damage.left = std::clamp<std::int64_t>(
        damage.left, 0, destination->width);
    damage.top = std::clamp<std::int64_t>(
        damage.top, 0, destination->height);
    damage.right = std::clamp<std::int64_t>(
        damage.right, damage.left, destination->width);
    damage.bottom = std::clamp<std::int64_t>(
        damage.bottom, damage.top, destination->height);
    const auto current_damage = damage;
    const auto previous_source = destination_scene_sources_.find(surface);
    if (const auto previous = destination_scene_damage_.find(surface);
        previous != destination_scene_damage_.end() &&
        previous_source != destination_scene_sources_.end() &&
        previous_source->second == source_surface) {
        damage.left = std::min(damage.left, previous->second.left);
        damage.top = std::min(damage.top, previous->second.top);
        damage.right = std::max(damage.right, previous->second.right);
        damage.bottom = std::max(damage.bottom, previous->second.bottom);
    }
    const auto width = damage.right - damage.left;
    const auto height = damage.bottom - damage.top;
    const auto pixel_count =
        static_cast<std::size_t>(width) * static_cast<std::size_t>(height);
    const std::vector<std::uint32_t> clear(pixel_count, 0xff000000U);
    // LayerKit retains unchanged sibling layers in the swap backing. Extend
    // invalidation across old and new bounds only while the same large source
    // moves. A replacement source starts a new scene generation; clearing the
    // outgoing source's larger bounds would erase retained siblings outside
    // the replacement's local bounds until their next dirty update.
    if (width > 0 && height > 0 &&
        write_region(*destination, damage.left, damage.top, width, height,
                     clear, call)) {
        destination_frame_sequences_[surface] = sequence;
        destination_scene_damage_[surface] = current_damage;
        destination_scene_sources_[surface] = source_surface;
    }
}

}  // namespace ilemu
