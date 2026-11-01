// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// Adapt guest framebuffer layer configuration, swaps and display
// notifications.

#include "kernel/mobile_framebuffer_hle.hpp"

#include <algorithm>
#include <atomic>
#include <bit>
#include <cmath>
#include <cstdint>
#include <limits>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "foundation/address_space.hpp"
#include "foundation/application_display.hpp"
#include "foundation/application_path.hpp"
#include "graphics/core_surface_abi.hpp"
#include "foundation/cpu.hpp"
#include "graphics/display.hpp"
#include "graphics/gles_renderer.hpp"
#include "graphics/gles_renderer_owner.hpp"
#include "kernel/iokit_abi.hpp"
#include "kernel/kernel_shared_state.hpp"
#include "graphics/mobile_framebuffer_abi.hpp"
#include "foundation/output.hpp"
#include "foundation/performance.hpp"
#include "graphics/presentation_tracker.hpp"
#include "foundation/scene_coordinator.hpp"
#include "graphics/surface_store.hpp"
#include "graphics/surface_transport_abi.hpp"
#include "foundation/userland_hle.hpp"
#include "surface_transport_hle.hpp"

namespace ilemu {
namespace {

    constexpr std::string_view framebuffer_image {
        "/IOMobileFramebuffer.framework/"
        "IOMobileFramebuffer"
    };
    std::atomic<std::uint64_t> next_scanout_surface { 1 };

    std::optional<ApplicationDisplay> application_display_for_process(
        const std::shared_ptr<KernelSharedState>& shared_state,
        std::uint32_t process_id)
    {
        if (!shared_state)
            return std::nullopt;
        std::lock_guard lock { shared_state->mach_mutex };
        const auto process = shared_state->processes.find(process_id);
        return process == shared_state->processes.end()
                   ? std::nullopt
                   : std::optional<ApplicationDisplay> {
                         process->second.application_display };
    }

} // namespace

MobileFramebufferHle::MobileFramebufferHle(UserlandHleRegistry& registry,
    std::shared_ptr<DisplayState> display,
    std::shared_ptr<SurfaceStore> surfaces,
    std::shared_ptr<PresentationTracker> presentations)
    : display_ { std::move(display) }
    , surface_store_ { surfaces ? std::move(surfaces)
                                : std::make_shared<SurfaceStore>() }
    , presentation_tracker_ { presentations
                                  ? std::move(presentations)
                                  : std::make_shared<PresentationTracker>() }
    , host_graphics_ { shared_gles_renderer() }
{
    reset_composition_targets();
    const auto add = [&](std::string symbol,
                         UserlandHleRegistry::Handler handler) {
        registry.register_function(std::string { framebuffer_image },
            std::move(symbol),
            [this, handler = std::move(handler)](UserlandHleCall& call) {
                const auto entry_symbol = call.symbol();
                if (entry_symbol.starts_with("_IOMobileFramebufferSwap") ||
                    entry_symbol == "_IOMobileFramebufferWaitSurface")
                    enter_swap_call(call, handler);
                else
                    handler(call);
            });
    };
    register_device_functions(registry);
    // Observe the firmware callback before it coalesces received notifications.
    // Some implementations expose the count query; others keep it inside the
    // native C callback. Both retain firmware dispatch and object lifetime.
    const auto observe_callback = [this](UserlandHleCall& call) {
        const auto callback_processor =
            static_cast<std::uint32_t>(call.cpu().processor_id());
        std::optional<std::uint64_t> callback_sequence;
        if (shared_state_) {
            std::lock_guard lock { shared_state_->mach_mutex };
            // This records the processor and consumes the host-only pending
            // watermark at the real firmware callback boundary, after Mach
            // delivery and before the original routine resumes. Guest
            // callback semantics are unchanged.
            callback_sequence =
                shared_state_->observe_display_vsync_callback_locked(
                call.process_id(), call.argument(0), callback_processor);
        }
        performance_counters().record_vsync_callback(call.process_id(),
            call.argument(0), callback_sequence.value_or(0),
            callback_processor);
        call.resume_original_persistently();
    };
    add("_IOMobileFramebufferGetNotifyMessageCount", observe_callback);
    registry.register_function(std::string { framebuffer_image },
        "_IOMobileFramebufferVsyncNotifyFunc", observe_callback,
        UserlandHleRegistry::SymbolLookup::ImageAndCacheLocals);
    // GetLayerDefaultSurface intentionally remains firmware code. It calls
    // IOConnectCallScalarMethod(selector 3) and then CoreSurfaceBufferLookup,
    // preserving the real CFRuntime wrapper around our client-buffer HLE.
    add("_IOMobileFramebufferSwapBegin", [this](UserlandHleCall& call) {
        const auto callback_processor =
            static_cast<std::uint32_t>(call.cpu().processor_id());
        std::optional<std::uint64_t> callback_sequence;
        if (shared_state_) {
            std::lock_guard lock { shared_state_->mach_mutex };
            callback_sequence =
                shared_state_->observe_display_vsync_frame_begin_locked(
                    call.process_id(), call.argument(0), callback_processor);
        }
        if (callback_sequence) {
            performance_counters().record_vsync_callback(
                call.process_id(), call.argument(0), *callback_sequence,
                callback_processor);
        }
        const auto swap_id = next_swap_id_++;
        call.set_return(call.write32(call.argument(1), swap_id)
                            ? iokit_abi::success
                            : iokit_abi::bad_argument);
    });
    add("_IOMobileFramebufferSwapSetBackgroundColor",
        [this](UserlandHleCall& call) { set_background_color(call); });
    add("_IOMobileFramebufferSwapEnd", [this](UserlandHleCall& call) {
        if (display_write_allowed(call)) {
            if (shared_state_) {
                std::lock_guard lock { shared_state_->mach_mutex };
                shared_state_->observe_display_vsync_swap_end_locked(
                    call.process_id(), call.argument(0));
            }
            performance_counters().record_vsync_swap_end(call.process_id(),
                call.argument(0),
                static_cast<std::uint32_t>(call.cpu().processor_id()));
            submit_layers(call);
            return;
        }
        call.set_return(iokit_abi::success);
    });
    add("_IOMobileFramebufferSwapSurface", [this](UserlandHleCall& call) {
        if (display_ && display_write_allowed(call))
            display_->present(call.process_id());
        call.set_return(iokit_abi::success);
    });
    const auto success = [](UserlandHleCall& call) {
        call.set_return(iokit_abi::success);
    };
    add("_IOMobileFramebufferWaitSurface", success);
    add("_IOMobileFramebufferEnableStatistics", success);
    // GetNotifyMessageCount's observed original implementation asks the Mach
    // port layer for MACH_PORT_RECEIVE_STATUS and returns mps_msgcount so the
    // native callback can coalesce queued notifications accurately.
    add("_IOMobileFramebufferSetDebugFlags", success);
    add("_IOMobileFramebufferSetTVOutMode", success);
    add("_IOMobileFramebufferSetWSSInfo", success);
    add("_IOMobileFramebufferSwapSetGammaTable", success);
    add("_IOMobileFramebufferSwapSetLayer",
        [this](UserlandHleCall& call) { set_layer(call); });
    add("_IOMobileFramebufferSwapWait", success);
    // SwapEnd completes synchronously, so the timed wait observes the same
    // completed transaction as the untimed entry point.
    add("_IOMobileFramebufferSwapWaitWithTimeout", success);
    add("_IOMobileFramebufferCreateStatistics",
        [](UserlandHleCall& call) { call.set_return(0); });
}

void MobileFramebufferHle::reset()
{
    cancel_pending_work();
    sync_lease_.reset();
    external_framebuffers_.clear();
    layers_.clear();
    layer_surface_leases_.clear();
    // The panel is shared by all guest processes. An exec of a helper process
    // must not erase the panel's last submitted frame while SpringBoard is
    // being relaunched; preserve scanout until the new owner presents.
    next_swap_id_ = 1;
    background_argb_ = 0xff000000U;
    submitted_background_argb_ = background_argb_;
    reset_composition_targets();
    // Keep submitted_layers_ and scanout_contents_valid_ across process exec.
}

void MobileFramebufferHle::inherit_state(const MobileFramebufferHle& parent)
{
    cancel_pending_work();
    sync_lease_.reset();
    external_framebuffers_ = parent.external_framebuffers_;
    layers_ = parent.layers_;
    layer_surface_leases_ = parent.layer_surface_leases_;
    submitted_layers_ = parent.submitted_layers_;
    next_swap_id_ = parent.next_swap_id_;
    background_argb_ = parent.background_argb_;
    submitted_background_argb_ = parent.submitted_background_argb_;
    scanout_surface_ = parent.scanout_surface_;
    scanout_lease_ = parent.scanout_lease_;
    // The published scanout is read-only from the child's point of view. Do
    // not inherit the writable composition targets: forked kernels can submit
    // concurrently, so each instance must own the targets it may modify.
    reset_composition_targets();
    scanout_contents_valid_ = parent.scanout_contents_valid_;
}

void MobileFramebufferHle::set_display(std::shared_ptr<DisplayState> display)
{
    display_ = std::move(display);
    if (scanout_surface_ && display_) {
        const auto descriptor = scanout_surface_->descriptor();
        if (descriptor.width != display_->width() ||
            descriptor.height != display_->height()) {
            scanout_surface_.reset();
            scanout_lease_.reset();
            reset_composition_targets();
            submitted_layers_.clear();
            scanout_contents_valid_ = false;
        }
    }
}

void MobileFramebufferHle::set_shared_state(
    std::shared_ptr<KernelSharedState> shared_state)
{
    shared_state_ = std::move(shared_state);
}

void MobileFramebufferHle::set_presentation_tracker(
    std::shared_ptr<PresentationTracker> presentations)
{
    presentation_tracker_ = std::move(presentations);
}

void MobileFramebufferHle::set_scene_coordinator(
    std::shared_ptr<SceneCoordinator> scenes)
{
    scene_coordinator_ = std::move(scenes);
}

void MobileFramebufferHle::set_frame_presented_handler(
    std::function<void(std::uint32_t)> handler)
{
    frame_presented_handler_ = std::move(handler);
}

void MobileFramebufferHle::set_semantic_presentation_handler(
    std::function<void(std::uint32_t)> handler)
{
    semantic_presentation_handler_ = std::move(handler);
}

bool MobileFramebufferHle::display_write_allowed(UserlandHleCall& call) const
{
    if (is_external_framebuffer(call))
        return false;
    if (!shared_state_)
        return true;
    std::lock_guard lock { shared_state_->mach_mutex };
    const auto process = shared_state_->processes.find(call.process_id());
    if (process == shared_state_->processes.end() ||
        !is_application_executable_path(process->second.executable_path)) {
        return true;
    }
    if (is_setup_assistant_executable_path(process->second.executable_path))
        return true;
    return active_application_owns_display_locked(*shared_state_,
        call.process_id(),
        scene_coordinator_
            ? std::optional<bool> { scene_coordinator_
                      ->client_scene_presentable(call.process_id()) }
            : std::nullopt);
}

bool MobileFramebufferHle::has_active_layers() const
{
    return !layers_.empty();
}

void MobileFramebufferHle::apply_application_display(
    LayerState& state, std::uint32_t producer_process_id,
    std::uint32_t source_width, std::uint32_t source_height) const
{
    if (!display_ || producer_process_id == 0U)
        return;
    const auto profile = application_display_for_process(
        shared_state_, producer_process_id);
    if (!profile || profile->kind == ApplicationDisplayMode::Native)
        return;
    const auto output = display_->geometry();
    const auto logical = application_display_geometry(*profile, output);
    if (!logical.valid() || source_width == 0U || source_height == 0U)
        return;
    const auto is_origin = [](float value) { return value == 0.0F; };
    const auto is_full_rectangle = [&](const Rectangle& rectangle,
                                       DisplayGeometry geometry) {
        return is_origin(rectangle.x) && is_origin(rectangle.y) &&
               rectangle.width == static_cast<float>(geometry.width) &&
               rectangle.height == static_cast<float>(geometry.height);
    };
    if (!is_origin(state.source.x) || !is_origin(state.source.y) ||
        (!is_full_rectangle(state.destination, logical) &&
            !is_full_rectangle(state.destination, output))) {
        return;
    }
    const auto viewport = application_display_viewport(*profile, output);
    const auto width = std::min(logical.width, source_width);
    const auto height = std::min(logical.height, source_height);
    if (viewport.width == 0U || viewport.height == 0U || width == 0U ||
        height == 0U) {
        return;
    }
    state.source = { 0.0F, 0.0F, static_cast<float>(width),
        static_cast<float>(height) };
    state.destination = { static_cast<float>(viewport.x),
        static_cast<float>(viewport.y), static_cast<float>(viewport.width),
        static_cast<float>(viewport.height) };
}

bool MobileFramebufferHle::application_surface_allowed(
    std::uint32_t producer_process_id, std::uint64_t publication_sequence) const
{
    if (!shared_state_ || producer_process_id == 0U ||
        publication_sequence == 0U) {
        return true;
    }
    if (!shared_state_->application_fullscreen_surface_suppression_active.load(
            std::memory_order_acquire)) {
        return true;
    }
    std::lock_guard lock { shared_state_->mach_mutex };
    return !shared_state_
                ->suppressed_application_fullscreen_surface_publications
                .contains({ producer_process_id, publication_sequence });
}

void MobileFramebufferHle::reset_composition_targets()
{
    const auto owner = allocate_gles_renderer_owner();
    auto lifetime = std::make_shared<GlesRendererOwner>(host_graphics_, owner);
    composition_surfaces_.clear();
    composition_surface_index_ = 0;
    composition_owner_ = owner;
    composition_lifetime_ = std::move(lifetime);
}

bool MobileFramebufferHle::ensure_scanout_surface()
{
    if (!display_)
        return false;
    if (scanout_surface_) {
        const auto descriptor = scanout_surface_->descriptor();
        if (descriptor.width == display_->width() &&
            descriptor.height == display_->height()) {
            return true;
        }
    }
    if (!composition_lifetime_->activate())
        return false;
    const auto pixels =
        static_cast<std::size_t>(display_->width()) * display_->height();
    const std::vector<std::uint32_t> initial(pixels, background_argb_);
    auto surface = host_graphics_->create_surface(
        { composition_owner_,
            next_scanout_surface.fetch_add(1, std::memory_order_relaxed) },
        HostSurfaceDescriptor { display_->width(), display_->height(),
            display_->width() * core_surface_abi::bytes_per_bgra_pixel,
            surface_pixel_format_bgra, PerfSurfaceKind::Scanout },
        initial);
    auto lease = make_host_surface_presentation_lease(
        surface, composition_lifetime_);
    scanout_surface_ = std::move(surface);
    scanout_lease_ = std::move(lease);
    submitted_layers_.clear();
    submitted_background_argb_ = background_argb_;
    scanout_contents_valid_ = false;
    return static_cast<bool>(scanout_surface_);
}

std::shared_ptr<HostSurface> MobileFramebufferHle::acquire_composition_surface()
{
    if (!composition_lifetime_->activate())
        return { };
    constexpr std::size_t composition_ring_size = 8;
    const auto descriptor =
        HostSurfaceDescriptor { display_->width(), display_->height(),
            display_->width() * core_surface_abi::bytes_per_bgra_pixel,
            surface_pixel_format_bgra, PerfSurfaceKind::Scanout };
    while (composition_surfaces_.size() < composition_ring_size) {
        auto surface = host_graphics_->create_surface(
            { composition_owner_,
                next_scanout_surface.fetch_add(1, std::memory_order_relaxed) },
            descriptor);
        composition_surfaces_.push_back(std::move(surface));
    }
    for (std::size_t attempt = 0; attempt < composition_surfaces_.size();
        ++attempt) {
        const auto index =
            composition_surface_index_ % composition_surfaces_.size();
        composition_surface_index_ =
            (index + 1U) % composition_surfaces_.size();
        const auto& candidate = composition_surfaces_[index];
        if (candidate && candidate != scanout_surface_ &&
            !candidate->presentation_leased())
            return candidate;
    }
    // All ring targets may still be referenced by the ordered presenter. Grow
    // the host-only pool for this submission instead of reusing a mutable
    // surface and changing the pixels of an older queued DisplayFrame.
    auto surface = host_graphics_->create_surface(
        { composition_owner_,
            next_scanout_surface.fetch_add(1, std::memory_order_relaxed) },
        descriptor);
    composition_surfaces_.push_back(surface);
    return surface;
}


void MobileFramebufferHle::set_layer(UserlandHleCall& call)
{
    const auto layer = call.argument(1);
    const auto surface = call.argument(2);
    if (layer > mobile_framebuffer_abi::maximum_layer_index) {
        call.set_return(iokit_abi::bad_argument);
        return;
    }
    if (is_external_framebuffer(call)) {
        call.set_return(iokit_abi::success);
        return;
    }
    if (surface == 0) {
        layers_.erase(layer);
        layer_surface_leases_.erase(layer);
        call.set_return(iokit_abi::success);
        return;
    }
    const auto identifier = surface_transport::public_surface_identifier(
        call, *surface_store_, surface);
    if (!identifier) {
        call.set_return(iokit_abi::bad_argument);
        return;
    }
    const auto surface_lease =
        surface_store_->acquire_transport_lease(*identifier);
    if (!surface_lease) {
        call.set_return(iokit_abi::bad_argument);
        return;
    }
    const auto float_argument = [&](std::size_t index) {
        return std::bit_cast<float>(call.argument(index));
    };
    const Rectangle source { float_argument(
                                 mobile_framebuffer_abi::source_x_argument),
        float_argument(mobile_framebuffer_abi::source_y_argument),
        float_argument(mobile_framebuffer_abi::source_width_argument),
        float_argument(mobile_framebuffer_abi::source_height_argument) };
    const Rectangle destination {
        float_argument(mobile_framebuffer_abi::destination_x_argument),
        float_argument(mobile_framebuffer_abi::destination_y_argument),
        float_argument(mobile_framebuffer_abi::destination_width_argument),
        float_argument(mobile_framebuffer_abi::destination_height_argument)
    };
    const auto valid_rectangle = [](const Rectangle& rectangle) {
        return std::isfinite(rectangle.x) && std::isfinite(rectangle.y) &&
               std::isfinite(rectangle.width) &&
               std::isfinite(rectangle.height) && rectangle.width > 0.0F &&
               rectangle.height > 0.0F;
    };
    if (!valid_rectangle(source) || !valid_rectangle(destination)) {
        call.set_return(iokit_abi::bad_argument);
        return;
    }
    layers_.insert_or_assign(
        layer, LayerState { *identifier, source, destination,
                   call.argument(mobile_framebuffer_abi::flags_argument) });
    layer_surface_leases_.insert_or_assign(layer, surface_lease);
    call.set_return(iokit_abi::success);
}

std::optional<std::uint32_t> MobileFramebufferHle::record_presentation(
    UserlandHleCall& call)
{
    if (!presentation_tracker_)
        return std::nullopt;
    std::vector<PresentationLayer> presented_layers;
    presented_layers.reserve(layers_.size());
    for (const auto& [order, state] : layers_) {
        const auto backing = surface_store_->find(state.surface_id);
        if (!backing || !application_surface_allowed(
                            backing->provenance.producer_process_id,
                            backing->provenance.publication_sequence))
            continue;
        auto effective_state = state;
        apply_application_display(effective_state,
            backing->provenance.producer_process_id, backing->width,
            backing->height);
        const auto scale_x =
            effective_state.source.width / effective_state.destination.width;
        const auto scale_y =
            effective_state.source.height / effective_state.destination.height;
        presented_layers.push_back(PresentationLayer { order, state.surface_id,
            backing->provenance,
            PresentationRectangle { effective_state.source.x,
                effective_state.source.y, effective_state.source.width,
                effective_state.source.height },
            PresentationRectangle { effective_state.destination.x,
                effective_state.destination.y, effective_state.destination.width,
                effective_state.destination.height },
            PresentationTransform { scale_x, 0.0F, 0.0F, scale_y,
                effective_state.source.x -
                    effective_state.destination.x * scale_x,
                effective_state.source.y -
                    effective_state.destination.y * scale_y },
            effective_state.flags });
    }
    auto logical_client_scene = scene_coordinator_
                                    ? scene_coordinator_->active_client_scene()
                                    : std::nullopt;
    const auto semantic_process_id =
        logical_client_scene && !presented_layers.empty() &&
                logical_client_scene->state == ClientSceneState::Active
            ? std::optional<std::uint32_t> { logical_client_scene
                      ->client_process_id }
            : std::nullopt;
    static_cast<void>(presentation_tracker_->record(call.process_id(),
        std::move(presented_layers), std::move(logical_client_scene)));
    return semantic_process_id;
}

void MobileFramebufferHle::set_background_color(UserlandHleCall& call)
{
    if (is_external_framebuffer(call)) {
        call.set_return(iokit_abi::success);
        return;
    }
    const auto channel = [](std::uint32_t bits) {
        const auto value = std::clamp(std::bit_cast<float>(bits), 0.0F, 1.0F);
        return static_cast<std::uint32_t>(std::lround(value * 255.0F));
    };
    const auto red = channel(call.argument(1));
    const auto green = channel(call.argument(2));
    const auto blue = channel(call.argument(3));
    const auto alpha = channel(call.argument(4));
    background_argb_ = (alpha << 24U) | (red << 16U) | (green << 8U) | blue;
    // This updates the pending swap transaction.  Publishing the clear here
    // exposes an incomplete frame when SpringBoard sets the background before
    // its layers and SwapEnd, notably during Lock/wake handoff.
    call.set_return(iokit_abi::success);
}

} // namespace ilemu
