// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "kernel/mobile_framebuffer_hle.hpp"

#include "foundation/cpu.hpp"
#include "foundation/output.hpp"
#include "foundation/userland_hle.hpp"
#include "graphics/display.hpp"
#include "graphics/gles_renderer.hpp"
#include "graphics/surface_store.hpp"
#include "kernel/graphics_call_lease.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <string>
#include <utility>

namespace ilemu {

struct MobileFramebufferHle::NativeSubmission {
    struct Work {
        // Only this host-owned packet crosses the queue. Credit outlives inputs.
        std::shared_ptr<void> credit;
        std::shared_ptr<GlesRenderer> renderer;
        std::shared_ptr<HostSurfacePresentationLease> target_lease, previous_lease;
        std::shared_ptr<HostSurface> target, previous;
        struct Copy {
            std::shared_ptr<HostSurface> source;
            std::shared_ptr<SurfaceTransportLease> transport;
            HostRectangle source_rectangle, destination_rectangle, clip;
        };
        std::vector<Copy> copies;
        std::vector<HostRectangle> damage;
        std::uint32_t background { };

        bool execute() const
        {
            const auto encoder = renderer->create_command_encoder();
            if (!encoder)
                return false;
            const auto descriptor = target->descriptor();
            const HostRectangle full { 0, 0, descriptor.width, descriptor.height };
            if (previous && !encoder->copy(previous, target, full, full,
                    HostCompositeMode::Copy, 0xffU, HostFilter::Nearest,
                    HostRotation::Identity))
                return false;
            for (const auto rectangle : damage)
                if (!encoder->fill(target, rectangle, background))
                    return false;
            for (const auto& copy : copies)
                if (!encoder->copy(copy.source, target, copy.source_rectangle,
                        copy.destination_rectangle,
                        HostCompositeMode::PremultipliedSourceOver, 0xffU,
                        HostFilter::Nearest, HostRotation::Identity, copy.clip))
                    return false;
            return encoder->submit(PerfSubmitReason::Compositor);
        }
    };
    std::shared_ptr<Work> work;
    std::shared_ptr<GraphicsCallLease> lease;
    std::shared_ptr<GlesWorkResult> result;
    std::shared_ptr<DisplayState> display;
    std::map<std::uint32_t, SubmittedLayer> layers;
    std::uint64_t owner { };
};

MobileFramebufferHle::SubmissionResult
MobileFramebufferHle::submit_host_layers(UserlandHleCall& call)
{
    constexpr bool debug_submission = false;
    if (!display_ || !host_graphics_->accelerated())
        return SubmissionResult::Failed;
    if (!ensure_scanout_surface())
        return SubmissionResult::Failed;
    const auto exact_rectangle =
        [](const Rectangle& rectangle) -> std::optional<HostRectangle> {
        const auto exact = [](float value) {
            return std::isfinite(value) && std::trunc(value) == value;
        };
        if (!exact(rectangle.x) || !exact(rectangle.y) ||
            !exact(rectangle.width) || !exact(rectangle.height) ||
            rectangle.x < 0.0F || rectangle.y < 0.0F ||
            rectangle.width <= 0.0F || rectangle.height <= 0.0F ||
            static_cast<double>(rectangle.x) >
                static_cast<double>(std::numeric_limits<std::int32_t>::max()) ||
            static_cast<double>(rectangle.y) >
                static_cast<double>(std::numeric_limits<std::int32_t>::max()) ||
            static_cast<double>(rectangle.width) >
                static_cast<double>(
                    std::numeric_limits<std::uint32_t>::max()) ||
            static_cast<double>(rectangle.height) >
                static_cast<double>(
                    std::numeric_limits<std::uint32_t>::max())) {
            return std::nullopt;
        }
        return HostRectangle { static_cast<std::int32_t>(rectangle.x),
            static_cast<std::int32_t>(rectangle.y),
            static_cast<std::uint32_t>(rectangle.width),
            static_cast<std::uint32_t>(rectangle.height) };
    };

    struct PreparedLayer {
        std::uint32_t order { };
        LayerState state;
        std::shared_ptr<HostSurface> source;
        HostRectangle source_rectangle;
        HostRectangle destination_rectangle;
        std::uint64_t generation { };
    };
    std::vector<PreparedLayer> prepared_layers;
    prepared_layers.reserve(layers_.size());
    for (const auto& [layer, state] : layers_) {
        const auto backing = surface_store_->find(state.surface_id);
        const auto allowed =
            !backing || application_surface_allowed(
                            backing->provenance.producer_process_id,
                            backing->provenance.publication_sequence);
        if (!allowed) {
            continue;
        }
        auto effective_state = state;
        if (backing) {
            apply_application_display(effective_state,
                backing->provenance.producer_process_id, backing->width,
                backing->height);
        }
        const auto source =
            surface_store_->host_surface(effective_state.surface_id);
        if (debug_submission) {
            std::string message =
                "[mfb-debug] layer pid=" + std::to_string(call.process_id()) +
                " order=" + std::to_string(layer) +
                " id=" + std::to_string(state.surface_id) +
                " allowed=" + (allowed ? "1" : "0") +
                " src=" + std::to_string(state.source.x) + "," +
                std::to_string(state.source.y) + "," +
                std::to_string(state.source.width) + "," +
                std::to_string(state.source.height) +
                " dst=" + std::to_string(state.destination.x) + "," +
                std::to_string(state.destination.y) + "," +
                std::to_string(state.destination.width) + "," +
                std::to_string(state.destination.height);
            if (backing) {
                message += " backing=" + std::to_string(backing->width) +
                           "x" + std::to_string(backing->height) +
                           " pitch=" + std::to_string(backing->bytes_per_row) +
                           " fmt=" + std::to_string(backing->pixel_format) +
                           " producer=" +
                           std::to_string(backing->provenance.producer_process_id) +
                           " publication=" +
                           std::to_string(backing->provenance.publication_sequence);
            } else {
                message += " backing=missing";
            }
            if (source) {
                const auto descriptor = source->descriptor();
                message += " host=" + std::to_string(descriptor.width) + "x" +
                           std::to_string(descriptor.height) +
                           " hostfmt=" + std::to_string(descriptor.pixel_format) +
                           " cpu=" + std::to_string(source->cpu_generation()) +
                           " gpu=" + std::to_string(source->gpu_generation()) +
                           " damage=" +
                           std::to_string(source->cpu_damage_rectangles().size());
            } else {
                message += " host=missing";
            }
            call.output().write(message + "\n");
        }
        // Page-granular guest dirtiness cannot be merged safely into a surface
        // whose complete contents are newer on the GPU. CPU-owned layers still
        // need their direct mapped writes imported before presentation.
        if (source && !source->work_succeeded())
            return SubmissionResult::Failed;
        if (source && source->gpu_generation() <= source->cpu_generation() &&
            surface_store_->try_synchronize_from_guest(
                call.memory(), effective_state.surface_id) !=
                SurfaceStore::CpuImportResult::Completed) {
            return SubmissionResult::Failed;
        }
        const auto source_rectangle = exact_rectangle(effective_state.source);
        const auto destination_rectangle =
            exact_rectangle(effective_state.destination);
        if (!backing ||
            (backing->pixel_format != surface_pixel_format_bgra &&
                !surface_is_packed_555(backing->pixel_format)) ||
            !source || !source_rectangle || !destination_rectangle ||
            static_cast<std::uint32_t>(source_rectangle->x) >
                backing->width -
                    std::min(backing->width, source_rectangle->width) ||
            source_rectangle->width > backing->width ||
            static_cast<std::uint32_t>(source_rectangle->y) >
                backing->height -
                    std::min(backing->height, source_rectangle->height) ||
            source_rectangle->height > backing->height ||
            static_cast<std::uint32_t>(destination_rectangle->x) >
                display_->width() -
                    std::min(display_->width(), destination_rectangle->width) ||
            destination_rectangle->width > display_->width() ||
            static_cast<std::uint32_t>(destination_rectangle->y) >
                display_->height() - std::min(display_->height(),
                                         destination_rectangle->height) ||
            destination_rectangle->height > display_->height()) {
            return SubmissionResult::Failed;
        }
        prepared_layers.push_back(
            { layer, effective_state, source, *source_rectangle,
                *destination_rectangle,
                std::max(source->cpu_generation(), source->gpu_generation()) });
    }

    if (debug_submission) {
        call.output().write(
            "[mfb-debug] submit pid=" + std::to_string(call.process_id()) +
            " layers=" + std::to_string(layers_.size()) +
            " prepared=" + std::to_string(prepared_layers.size()) +
            " scanout-valid=" + (scanout_contents_valid_ ? "1" : "0") +
            " background=" + std::to_string(background_argb_) + "\n");
    }

    const auto right = [](const HostRectangle& rectangle) {
        return static_cast<std::int64_t>(rectangle.x) + rectangle.width;
    };
    const auto bottom = [](const HostRectangle& rectangle) {
        return static_cast<std::int64_t>(rectangle.y) + rectangle.height;
    };
    const auto intersection = [&](const HostRectangle& left,
                                  const HostRectangle& right_rectangle)
        -> std::optional<HostRectangle> {
        const auto x = std::max(left.x, right_rectangle.x);
        const auto y = std::max(left.y, right_rectangle.y);
        const auto x_end = std::min(right(left), right(right_rectangle));
        const auto y_end = std::min(bottom(left), bottom(right_rectangle));
        if (x_end <= x || y_end <= y)
            return std::nullopt;
        return HostRectangle { x, y, static_cast<std::uint32_t>(x_end - x),
            static_cast<std::uint32_t>(y_end - y) };
    };
    const auto full_display =
        HostRectangle { 0, 0, display_->width(), display_->height() };

    for (const auto& layer : prepared_layers)
        layer.source->mark_scanout_presentation();

    std::vector<HostRectangle> damage;
    const auto add_damage = [&](HostRectangle rectangle) {
        const auto clipped = intersection(rectangle, full_display);
        if (!clipped)
            return;
        rectangle = *clipped;
        for (std::size_t index = 0; index < damage.size();) {
            const auto touches =
                static_cast<std::int64_t>(rectangle.x) <=
                    right(damage[index]) &&
                static_cast<std::int64_t>(damage[index].x) <=
                    right(rectangle) &&
                static_cast<std::int64_t>(rectangle.y) <=
                    bottom(damage[index]) &&
                static_cast<std::int64_t>(damage[index].y) <= bottom(rectangle);
            if (!touches) {
                ++index;
                continue;
            }
            const auto x = std::min(rectangle.x, damage[index].x);
            const auto y = std::min(rectangle.y, damage[index].y);
            const auto x_end = std::max(right(rectangle), right(damage[index]));
            const auto y_end =
                std::max(bottom(rectangle), bottom(damage[index]));
            rectangle = { x, y, static_cast<std::uint32_t>(x_end - x),
                static_cast<std::uint32_t>(y_end - y) };
            damage.erase(damage.begin() + static_cast<std::ptrdiff_t>(index));
            index = 0;
        }
        damage.push_back(rectangle);
        // Keep command growth bounded if a pathological producer updates many
        // isolated regions in one SwapEnd. The fallback remains a conservative
        // union, not a layer-driven full-screen expansion.
        constexpr std::size_t maximum_damage_rectangles = 32;
        if (damage.size() > maximum_damage_rectangles) {
            auto merged = damage.front();
            for (std::size_t index = 1; index < damage.size(); ++index) {
                const auto x = std::min(merged.x, damage[index].x);
                const auto y = std::min(merged.y, damage[index].y);
                const auto x_end =
                    std::max(right(merged), right(damage[index]));
                const auto y_end =
                    std::max(bottom(merged), bottom(damage[index]));
                merged = { x, y, static_cast<std::uint32_t>(x_end - x),
                    static_cast<std::uint32_t>(y_end - y) };
            }
            damage.assign(1, merged);
        }
    };
    const auto propagate_source_damage =
        [&](HostRectangle source_damage, HostRectangle source_rectangle,
            HostRectangle destination_rectangle)
        -> std::optional<HostRectangle> {
        const auto affected = intersection(source_damage, source_rectangle);
        if (!affected)
            return std::nullopt;
        const auto relative_left =
            static_cast<std::uint64_t>(affected->x - source_rectangle.x);
        const auto relative_top =
            static_cast<std::uint64_t>(affected->y - source_rectangle.y);
        const auto relative_right =
            static_cast<std::uint64_t>(right(*affected) - source_rectangle.x);
        const auto relative_bottom =
            static_cast<std::uint64_t>(bottom(*affected) - source_rectangle.y);
        const auto scale_floor =
            [](std::uint64_t value, std::uint32_t destination,
                std::uint32_t source) { return value * destination / source; };
        const auto scale_ceil = [](std::uint64_t value,
                                    std::uint32_t destination,
                                    std::uint32_t source) {
            return (value * destination + source - 1U) / source;
        };
        const auto x =
            static_cast<std::int64_t>(destination_rectangle.x) +
            static_cast<std::int64_t>(scale_floor(relative_left,
                destination_rectangle.width, source_rectangle.width));
        const auto y =
            static_cast<std::int64_t>(destination_rectangle.y) +
            static_cast<std::int64_t>(scale_floor(relative_top,
                destination_rectangle.height, source_rectangle.height));
        const auto x_end =
            static_cast<std::int64_t>(destination_rectangle.x) +
            static_cast<std::int64_t>(scale_ceil(relative_right,
                destination_rectangle.width, source_rectangle.width));
        const auto y_end =
            static_cast<std::int64_t>(destination_rectangle.y) +
            static_cast<std::int64_t>(scale_ceil(relative_bottom,
                destination_rectangle.height, source_rectangle.height));
        if (x_end <= x || y_end <= y ||
            x < std::numeric_limits<std::int32_t>::min() ||
            y < std::numeric_limits<std::int32_t>::min() ||
            x > std::numeric_limits<std::int32_t>::max() ||
            y > std::numeric_limits<std::int32_t>::max()) {
            return std::nullopt;
        }
        return HostRectangle { static_cast<std::int32_t>(x),
            static_cast<std::int32_t>(y), static_cast<std::uint32_t>(x_end - x),
            static_cast<std::uint32_t>(y_end - y) };
    };
    if (!scanout_contents_valid_ ||
        submitted_background_argb_ != background_argb_) {
        add_damage(full_display);
    } else {
        for (const auto& [order, submitted] : submitted_layers_) {
            const auto current = std::find_if(prepared_layers.begin(),
                prepared_layers.end(), [order](const PreparedLayer& layer) {
                    return layer.order == order;
                });
            if (current == prepared_layers.end()) {
                if (const auto old_destination =
                        exact_rectangle(submitted.state.destination)) {
                    add_damage(*old_destination);
                }
                continue;
            }
            if (current->state != submitted.state ||
                current->source->key() != submitted.surface_key) {
                if (const auto old_destination =
                        exact_rectangle(submitted.state.destination)) {
                    add_damage(*old_destination);
                }
                add_damage(current->destination_rectangle);
            } else if (current->generation != submitted.generation) {
                const auto source_damage =
                    current->source->damage_since(submitted.generation);
                for (const auto rectangle : source_damage) {
                    if (const auto transformed = propagate_source_damage(
                            rectangle, current->source_rectangle,
                            current->destination_rectangle)) {
                        add_damage(*transformed);
                    }
                }
            }
        }
        for (const auto& current : prepared_layers) {
            if (!submitted_layers_.contains(current.order))
                add_damage(current.destination_rectangle);
        }
    }

    if (debug_submission) {
        call.output().write(
            "[mfb-debug] damage pid=" + std::to_string(call.process_id()) +
            " rects=" + std::to_string(damage.size()) +
            " submitted=" + std::to_string(submitted_layers_.size()) + "\n");
    }

    if (damage.empty()) {
        submitted_layers_.clear();
        for (const auto& layer : prepared_layers)
            submitted_layers_.emplace(layer.order,
                SubmittedLayer { layer.state, layer.source->key(), layer.generation });
        publish_native_scanout(call);
        return SubmissionResult::Submitted;
    }
    const auto copy_capacity = prepared_layers.size() * damage.size();
    const auto bytes = sizeof(NativeSubmission) + sizeof(NativeSubmission::Work) +
                       copy_capacity * sizeof(NativeSubmission::Work::Copy) +
                       damage.capacity() * sizeof(HostRectangle) +
                       prepared_layers.size() *
                           (sizeof(std::pair<const std::uint32_t, SubmittedLayer>) +
                               4 * sizeof(void*));
    auto credit = reserve_gles_host_preparation(bytes);
    const auto retry = [this, lease = sync_lease_](UserlandHleCall& resumed) {
        lease->consume();
        submit_layers(resumed);
    };
    if (!credit) {
        call.suspend_for_host(
            [bytes] { return gles_host_preparation_capacity(bytes); }, retry);
        return SubmissionResult::Pending;
    }
    auto pending = std::make_shared<NativeSubmission>();
    auto work = std::make_shared<NativeSubmission::Work>();
    pending->work = work;
    pending->lease = sync_lease_;
    pending->display = display_;
    pending->owner = composition_owner_;
    work->credit = std::move(credit);
    work->renderer = host_graphics_;
    work->target = acquire_composition_surface();
    if (!work->target)
        return SubmissionResult::Failed;
    work->target_lease = make_host_surface_presentation_lease(
        work->target, composition_lifetime_);
    if (scanout_contents_valid_) {
        work->previous = scanout_surface_;
        work->previous_lease = scanout_lease_;
    }
    work->background = background_argb_;
    work->damage = std::move(damage);
    work->copies.reserve(copy_capacity);
    for (const auto& layer : prepared_layers) {
        auto transport =
            surface_store_->acquire_transport_lease(layer.state.surface_id);
        if (!transport)
            return SubmissionResult::Failed;
        pending->layers.emplace(
            layer.order, SubmittedLayer { layer.state, layer.source->key(),
                             layer.generation });
        for (const auto rectangle : work->damage) {
            if (const auto clip = intersection(rectangle, layer.destination_rectangle))
                work->copies.push_back({ layer.source, transport,
                    layer.source_rectangle, layer.destination_rectangle, *clip });
        }
    }
    pending->result = enqueue_gles_surface_write(work->target,
        [work](DisplayFrame&) { return work->execute(); });
    if (!pending->result) {
        // Nothing was accepted. Rebuild live input preparation after capacity
        // returns, rather than retaining a mutable source across admission.
        call.suspend_for_host([] { return gles_host_work_capacity(0); }, retry);
        return SubmissionResult::Pending;
    }
    pending->lease->result = pending->result;
    // Core publication is serialized: no later guest writer can enter between
    // acceptance and these reads. Completion before registration is harmless.
    const auto completion = pending->result->completion();
    for (const auto& copy : work->copies)
        copy.source->record_read(completion);
    await_native_layers(call, std::move(pending));
    return SubmissionResult::Pending;
}

void MobileFramebufferHle::await_native_layers(
    UserlandHleCall& call, std::shared_ptr<NativeSubmission> pending)
{
    const auto result = pending->result;
    call.suspend_for_host([result] { return result->ready(); },
        [this, pending](UserlandHleCall& resumed) {
            try {
                bool live = pending->lease == sync_lease_ &&
                            !pending->lease->cancelled && !pending->lease->consumed &&
                            pending->owner == composition_owner_ &&
                            pending->display == display_ &&
                            pending->result->succeeded() && display_write_allowed(resumed);
                for (const auto& [order, layer] : pending->layers) {
                    static_cast<void>(order);
                    const auto backing = surface_store_->find(layer.state.surface_id);
                    const auto source = surface_store_->host_surface(layer.state.surface_id);
                    live = live && backing && source && source->key() == layer.surface_key &&
                           application_surface_allowed(
                               backing->provenance.producer_process_id,
                               backing->provenance.publication_sequence);
                }
                if (!live) {
                    finish_swap(resumed, pending->lease, false);
                    return;
                }
                scanout_surface_ = pending->work->target;
                scanout_lease_ = pending->work->target_lease;
                submitted_layers_ = std::move(pending->layers);
                submitted_background_argb_ = pending->work->background;
                scanout_contents_valid_ = true;
                publish_native_scanout(resumed);
                finish_swap(resumed, pending->lease, true);
            } catch (...) {
                finish_swap(resumed, pending->lease, false);
            }
        }, result->sequence());
}

void MobileFramebufferHle::publish_native_scanout(UserlandHleCall& call)
{
    auto graphics = host_graphics_;
    auto scanout = scanout_surface_;
    display_->replace_surface(
        scanout,
        [graphics, scanout] {
            if (!graphics->map_cpu(
                    *scanout, true, PerfCpuMapReason::DeferredDisplayRead))
                return std::vector<std::uint32_t> { };
            auto mapping = scanout->map_cpu(
                false, PerfCpuMapReason::DeferredDisplayRead);
            return mapping.frame().pixels;
        },
        call.process_id(),
        [scanout] {
            return std::max(
                scanout->cpu_generation(), scanout->gpu_generation());
        }, scanout_lease_);
}

} // namespace ilemu
