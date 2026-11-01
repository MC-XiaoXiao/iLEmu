// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "kernel/mobile_framebuffer_hle.hpp"

#include "foundation/address_space.hpp"
#include "foundation/cpu.hpp"
#include "foundation/performance.hpp"
#include "foundation/userland_hle.hpp"
#include "graphics/display.hpp"
#include "graphics/gles_program_renderer.hpp"
#include "graphics/gles_renderer.hpp"
#include "graphics/surface_store.hpp"
#include "kernel/graphics_call_lease.hpp"
#include "kernel/iokit_abi.hpp"
#include "surface_guest_import.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <utility>

namespace ilemu {
namespace {
    void fail_swap(
        UserlandHleCall& call, const std::shared_ptr<GraphicsCallLease>& lease)
    {
        lease->cancelled = true;
        if (lease->ready())
            lease->consume();
        call.set_return(iokit_abi::aborted);
    }
}

struct MobileFramebufferHle::SoftwareSubmission {
    // All retained CPU images share the existing preparation budget. Only
    // one source is materialized at a time, independently of GPU result credit.
    std::shared_ptr<void> credit;
    std::shared_ptr<GraphicsCallLease> lease;
    std::shared_ptr<DisplayState> display;
    struct Entry {
        LayerState state;
        std::uint32_t width { }, height { }, base { }, size { }, pitch { },
            format { };
        std::uint64_t publication { };
        std::shared_ptr<HostSurface> surface;
        std::shared_ptr<SurfaceTransportLease> transport;
        AddressSpace::MappingObservation mapping;
    };
    std::vector<Entry> entries;
    std::vector<std::uint32_t> pixels;
    std::size_t bytes { }, next { };

    void blend(const Entry& entry, std::vector<std::uint32_t>& source_pixels)
    {
        const auto geometry = display->geometry();
        const auto blend_channel = [](std::uint32_t source,
                                       std::uint32_t destination,
                                       std::uint32_t inverse_alpha) {
            return std::min(
                255U, source + (destination * inverse_alpha + 127U) / 255U);
        };
        const auto clipped_edge = [](double value, std::uint32_t maximum) {
            return static_cast<int>(std::clamp(
                std::floor(value + 0.5), 0.0, static_cast<double>(maximum)));
        };
        const auto destination_left =
            clipped_edge(entry.state.destination.x, geometry.width);
        const auto destination_top =
            clipped_edge(entry.state.destination.y, geometry.height);
        const auto destination_right =
            clipped_edge(static_cast<double>(entry.state.destination.x) +
                             entry.state.destination.width,
                geometry.width);
        const auto destination_bottom =
            clipped_edge(static_cast<double>(entry.state.destination.y) +
                             entry.state.destination.height,
                geometry.height);
        if (destination_right <= destination_left ||
            destination_bottom <= destination_top) {
            return;
        }
        const auto full_surface_copy =
            destination_left == 0 && destination_top == 0 &&
            destination_right == static_cast<int>(geometry.width) &&
            destination_bottom == static_cast<int>(geometry.height) &&
            entry.state.destination.x == 0.0F &&
            entry.state.destination.y == 0.0F &&
            entry.state.destination.width ==
                static_cast<float>(geometry.width) &&
            entry.state.destination.height ==
                static_cast<float>(geometry.height) &&
            entry.state.source.x == 0.0F && entry.state.source.y == 0.0F &&
            entry.state.source.width == static_cast<float>(entry.width) &&
            entry.state.source.height == static_cast<float>(entry.height) &&
            entry.width == geometry.width && entry.height == geometry.height &&
            std::all_of(source_pixels.begin(), source_pixels.end(),
                [](std::uint32_t pixel) { return (pixel >> 24U) == 255U; });
        if (full_surface_copy) {
            pixels = std::move(source_pixels);
            return;
        }
        std::vector<std::uint32_t> source_columns(
            static_cast<std::size_t>(destination_right - destination_left));
        for (auto x = destination_left; x < destination_right; ++x) {
            const auto horizontal =
                (static_cast<float>(x) + 0.5F - entry.state.destination.x) /
                entry.state.destination.width;
            source_columns[static_cast<std::size_t>(x - destination_left)] =
                static_cast<std::uint32_t>(std::floor(
                    std::clamp(static_cast<double>(entry.state.source.x) +
                                   horizontal * entry.state.source.width,
                        0.0, static_cast<double>(entry.width - 1U))));
        }
        for (auto y = destination_top; y < destination_bottom; ++y) {
            const auto vertical =
                (static_cast<float>(y) + 0.5F - entry.state.destination.y) /
                entry.state.destination.height;
            const auto source_y = static_cast<std::uint32_t>(std::floor(
                std::clamp(static_cast<double>(entry.state.source.y) +
                               vertical * entry.state.source.height,
                    0.0, static_cast<double>(entry.height - 1U))));
            for (auto x = destination_left; x < destination_right; ++x) {
                const auto source_x = source_columns[static_cast<std::size_t>(
                    x - destination_left)];
                const auto source_pixel =
                    source_pixels[static_cast<std::size_t>(source_y) *
                                      entry.width +
                                  static_cast<std::size_t>(source_x)];
                auto& destination_pixel =
                    pixels[static_cast<std::size_t>(y) * geometry.width +
                           static_cast<std::size_t>(x)];
                const auto alpha = source_pixel >> 24U;
                if (alpha == 255U) {
                    destination_pixel = source_pixel;
                    continue;
                }
                const auto inverse_alpha = 255U - alpha;
                const auto output_alpha = blend_channel(
                    alpha, destination_pixel >> 24U, inverse_alpha);
                const auto output_red =
                    blend_channel((source_pixel >> 16U) & 0xffU,
                        (destination_pixel >> 16U) & 0xffU, inverse_alpha);
                const auto output_green =
                    blend_channel((source_pixel >> 8U) & 0xffU,
                        (destination_pixel >> 8U) & 0xffU, inverse_alpha);
                const auto output_blue = blend_channel(source_pixel & 0xffU,
                    destination_pixel & 0xffU, inverse_alpha);
                destination_pixel = (output_alpha << 24U) |
                                    (output_red << 16U) | (output_green << 8U) |
                                    output_blue;
            }
        }
    }
};

void MobileFramebufferHle::service_graphics_completions()
{
    if (sync_lease_ && sync_lease_->cancelled && sync_lease_->ready()) {
        sync_lease_->consume();
        sync_lease_.reset();
    }
}

void MobileFramebufferHle::cancel_pending_work()
{
    if (sync_lease_)
        sync_lease_->cancelled = true;
}

void MobileFramebufferHle::retire_thread(std::size_t processor)
{
    if (sync_lease_ && sync_lease_->owner == processor)
        sync_lease_->cancelled = true;
}

void MobileFramebufferHle::enter_swap_call(
    UserlandHleCall& call, std::function<void(UserlandHleCall&)> handler)
{
    service_graphics_completions();
    if (sync_lease_ && !sync_lease_->consumed) {
        const auto lease = sync_lease_;
        if (call.symbol() == "_IOMobileFramebufferSwapBegin") {
            const auto output = call.argument(1);
            const auto mapping =
                call.memory().observe_mapping(output, sizeof(std::uint32_t));
            handler = [handler = std::move(handler), mapping, output](
                          UserlandHleCall& ready) {
                if (!mapping.valid() ||
                    !ready.memory().accessible(output, sizeof(std::uint32_t),
                        MemoryPermission::Write)) {
                    ready.set_return(iokit_abi::bad_argument);
                    return;
                }
                handler(ready);
            };
        }
        call.suspend_until([lease] { return lease->ready(); },
            [this, handler = std::move(handler)](UserlandHleCall& resumed) {
                enter_swap_call(resumed, handler);
            },
            lease->result ? lease->result->sequence() : 0);
        return;
    }
    sync_lease_.reset();
    handler(call);
}

void MobileFramebufferHle::finish_swap(UserlandHleCall& call,
    const std::shared_ptr<GraphicsCallLease>& lease, bool succeeded)
{
    if (lease != sync_lease_ || lease->cancelled || lease->consumed) {
        fail_swap(call, lease);
        return;
    }
    lease->consume();
    if (!succeeded) {
        call.set_return(iokit_abi::aborted);
        return;
    }
    const auto semantic_process_id = record_presentation(call);
    if (display_ && display_write_allowed(call)) {
        display_->present(call.process_id());
        performance_counters().record_vsync_guest_submit(
            call.process_id(), call.argument(0));
        if (frame_presented_handler_)
            frame_presented_handler_(call.process_id());
        if (semantic_process_id && semantic_presentation_handler_)
            semantic_presentation_handler_(*semantic_process_id);
    }
    call.set_return(iokit_abi::success);
}

void MobileFramebufferHle::submit_layers(UserlandHleCall& call)
{
    auto lease = std::make_shared<GraphicsCallLease>();
    lease->owner = call.cpu().processor_id();
    sync_lease_ = lease;
    try {
        if (!display_ || !display_write_allowed(call)) {
            finish_swap(call, lease, false);
            return;
        }
        if (submit_host_layers(call) != SubmissionResult::Failed) {
            finish_swap(call, lease, true);
            return;
        }
        auto pending = std::make_shared<SoftwareSubmission>();
        pending->lease = lease;
        pending->display = display_;
        constexpr std::size_t limit = 64U * 1024U * 1024U;
        const auto count = display_->geometry().pixel_count();
        if (count > limit / sizeof(std::uint32_t))
            throw std::length_error {
                "framebuffer composition exceeds 64 MiB"
            };
        std::size_t source_bytes = 0;
        for (const auto& [layer, state] : layers_) {
            static_cast<void>(layer);
            const auto backing = surface_store_->find(state.surface_id);
            if (!backing || !backing->width || !backing->height ||
                !application_surface_allowed(
                    backing->provenance.producer_process_id,
                    backing->provenance.publication_sequence))
                continue;
            const auto pixels =
                static_cast<std::uint64_t>(backing->width) * backing->height;
            if (pixels > limit / sizeof(std::uint32_t))
                throw std::length_error { "framebuffer source exceeds 64 MiB" };
            source_bytes = std::max(source_bytes,
                static_cast<std::size_t>(pixels) * sizeof(std::uint32_t));
            auto effective = state;
            apply_application_display(effective,
                backing->provenance.producer_process_id, backing->width,
                backing->height);
            auto transport =
                surface_store_->acquire_transport_lease(backing->id);
            if (!transport)
                throw std::runtime_error { "missing framebuffer source owner" };
            pending->entries.push_back({ effective, backing->width,
                backing->height, backing->base, backing->allocation_size,
                backing->bytes_per_row, backing->pixel_format,
                backing->provenance.publication_sequence,
                surface_store_->host_surface(backing->id), std::move(transport),
                call.memory().observe_mapping(
                    backing->base, backing->allocation_size) });
        }
        pending->bytes =
            (count + display_->width()) * sizeof(std::uint32_t) +
            sizeof(SoftwareSubmission) +
            pending->entries.size() * sizeof(SoftwareSubmission::Entry);
        if (pending->bytes > limit || source_bytes > limit - pending->bytes)
            throw std::length_error { "framebuffer inputs exceed 64 MiB" };
        pending->bytes += source_bytes;
        await_software_layers(call, std::move(pending));
    } catch (...) {
        fail_swap(call, lease);
    }
}

void MobileFramebufferHle::await_software_layers(
    UserlandHleCall& call, std::shared_ptr<SoftwareSubmission> pending)
try {
    if (sync_lease_ != pending->lease || pending->lease->cancelled ||
        pending->lease->consumed || display_ != pending->display ||
        !display_write_allowed(call))
        throw std::runtime_error { "stale framebuffer submission" };
    for (const auto& entry : pending->entries) {
        const auto current = surface_store_->find(entry.state.surface_id);
        if (!current || !entry.mapping.valid() || current->base != entry.base ||
            current->allocation_size != entry.size ||
            current->width != entry.width || current->height != entry.height ||
            current->bytes_per_row != entry.pitch ||
            current->pixel_format != entry.format ||
            !call.memory().accessible(
                entry.base, entry.size, MemoryPermission::Read) ||
            current->provenance.publication_sequence != entry.publication ||
            surface_store_->host_surface(entry.state.surface_id) !=
                entry.surface)
            throw std::runtime_error { "framebuffer input was replaced" };
    }
    if (!pending->credit) {
        pending->credit = reserve_gles_host_preparation(pending->bytes);
        if (!pending->credit) {
            call.suspend_until(
                [bytes = pending->bytes] {
                    return gles_host_preparation_capacity(bytes);
                },
                [this, pending](UserlandHleCall& resumed) {
                    await_software_layers(resumed, pending);
                });
            return;
        }
        pending->pixels.assign(
            display_->geometry().pixel_count(), background_argb_);
    }
    if (pending->next == pending->entries.size()) {
        scanout_contents_valid_ = false;
        submitted_layers_.clear();
        display_->replace_pixels(std::move(pending->pixels), call.process_id());
        finish_swap(call, pending->lease, true);
        return;
    }
    const auto& entry = pending->entries[pending->next];
    if (entry.surface)
        entry.surface->mark_scanout_presentation();
    SurfaceGuestImport::read(
        call, surface_store_, entry.state.surface_id,
        [this, pending](UserlandHleCall& resumed,
            std::optional<std::vector<std::uint32_t>> pixels) {
            try {
                const auto& source = pending->entries[pending->next];
                if (!pixels ||
                    pixels->size() != static_cast<std::uint64_t>(source.width) *
                                          source.height)
                    throw std::runtime_error { "framebuffer readback failed" };
                pending->blend(source, *pixels);
                ++pending->next;
                pixels.reset();
                await_software_layers(resumed, pending);
            } catch (...) {
                fail_swap(resumed, pending->lease);
            }
        },
        pending->lease);
} catch (...) {
    fail_swap(call, pending->lease);
}

} // namespace ilemu
