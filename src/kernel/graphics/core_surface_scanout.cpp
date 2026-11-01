// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "kernel/core_surface_hle.hpp"

#include "foundation/address_space.hpp"
#include "foundation/application_display.hpp"
#include "graphics/display.hpp"
#include "graphics/gles_program_renderer.hpp"
#include "graphics/surface_cpu_readback.hpp"
#include "graphics/surface_store.hpp"
#include "kernel/iokit_abi.hpp"
#include "kernel/kernel_shared_state.hpp"

#include <algorithm>
#include <utility>

namespace ilemu {

struct CoreSurfaceHle::ScanoutRead {
    SurfaceStore::Backing backing;
    AddressSpace::MappingObservation mapping;
    std::uint32_t owner { };
    std::uint64_t generation { };
    std::size_t bytes { };
    std::shared_ptr<GlesValueResult<SurfaceCpuReadback>> host;
};

bool CoreSurfaceHle::default_scanout_pending() const
{
    return static_cast<bool>(pending_scanout_);
}

bool CoreSurfaceHle::default_scanout_ready() const
{
    if (!pending_scanout_)
        return false;
    const auto& host = pending_scanout_->host;
    return host->completion ? host->completion->ready()
                            : gles_host_work_capacity(pending_scanout_->bytes);
}

void CoreSurfaceHle::discard_default_scanout()
{
    // No callback is installed in the worker. Dropping the core observer does
    // not cancel real GPU work or release its host-owned surface/result early.
    pending_scanout_.reset();
}

bool CoreSurfaceHle::refresh_default_scanout(
    AddressSpace& memory, std::uint32_t owner_process_id)
try {
    if (!display_)
        return false;
    const auto backing =
        surfaces_->find(iokit_abi::mobile_framebuffer_default_surface_id);
    if (!backing) {
        discard_default_scanout();
        return false;
    }
    const auto generation =
        memory.range_write_generation(backing->base, backing->allocation_size);
    const auto surface = surfaces_->host_surface(backing->id);
    const auto gpu_generation = surface ? surface->gpu_generation() : 0;
    if (!generation) {
        discard_default_scanout();
        return false;
    }
    const auto publication = backing->provenance.publication_sequence;
    std::vector<std::uint32_t> pixels;
    if (pending_scanout_) {
        const auto pending = pending_scanout_;
        if (pending->owner != owner_process_id || !pending->mapping.valid() ||
            pending->backing.provenance.publication_sequence != publication ||
            pending->backing.base != backing->base ||
            pending->backing.allocation_size != backing->allocation_size ||
            pending->backing.width != backing->width ||
            pending->backing.height != backing->height ||
            pending->backing.bytes_per_row != backing->bytes_per_row ||
            pending->backing.pixel_format != backing->pixel_format ||
            pending->host->value.surface != surface) {
            discard_default_scanout();
            return false;
        }
        if (!pending->host->completion) {
            try {
                const auto host = pending->host;
                host->completion = enqueue_gles_host_work(pending->bytes,
                    [host](DisplayFrame&) { return host->value.capture(); });
            } catch (...) {
                discard_default_scanout();
            }
            return false;
        }
        const auto& host = pending->host;
        if (!host->completion->ready())
            return false;
        const auto& captured = host->value.snapshot;
        last_scanout_publication_ = publication;
        last_scanout_generation_ = pending->generation;
        last_scanout_gpu_generation_ = gpu_generation;
        discard_default_scanout();
        if (!host->completion->succeeded() ||
            captured.frame.width != backing->width ||
            captured.frame.height != backing->height ||
            captured.frame.pixels.size() !=
                static_cast<std::size_t>(backing->width) * backing->height)
            return false;
        // Keep subsequent GPU writes observable even if guest pages did not
        // change. The captured frame remains an actual completed boundary.
        last_scanout_gpu_generation_ = captured.gpu_generation;
        if (surface->commit_cpu_readback(captured)) {
            host->value.renderer->acknowledge_readback(
                surface->key(), host->value.stamp);
            // Merge guest CPU writes made during the GPU wait. A later GPU
            // writer makes this return PendingGpu; keep the completed boundary
            // and leave the newer generations visible for the next refresh.
            auto merged = surfaces_->try_read_argb(memory, backing->id);
            if (merged.state == SurfaceStore::CpuReadResult::State::Completed) {
                pixels = std::move(merged.pixels);
                last_scanout_generation_ = generation;
                last_scanout_gpu_generation_ = merged.gpu_generation;
            }
        }
        if (pixels.empty())
            pixels = std::move(host->value.snapshot.frame.pixels);
    } else {
        if (surface->work_succeeded() &&
            last_scanout_publication_ == publication &&
            last_scanout_generation_ == generation &&
            last_scanout_gpu_generation_ == gpu_generation)
            return false;
        constexpr std::size_t limit = 64U * 1024U * 1024U;
        const auto count =
            static_cast<std::uint64_t>(backing->width) * backing->height;
        if (count > limit / sizeof(std::uint32_t))
            return false;
        auto read = surfaces_->try_read_argb(memory, backing->id);
        if (read.state == SurfaceStore::CpuReadResult::State::PendingGpu) {
            auto pending = std::make_shared<ScanoutRead>();
            pending->backing = *backing;
            pending->owner = owner_process_id;
            pending->generation = *generation;
            pending->bytes =
                static_cast<std::size_t>(count) * sizeof(std::uint32_t);
            pending->mapping =
                memory.observe_mapping(backing->base, backing->allocation_size);
            if (!pending->mapping.valid())
                return false;
            pending->host =
                std::make_shared<GlesValueResult<SurfaceCpuReadback>>();
            pending->host->value.renderer = shared_gles_renderer();
            pending->host->value.surface = surface;
            pending_scanout_ = std::move(pending);
            // Admission and later completion share the existing native wake.
            // No guest caller exists for this periodic device refresh.
            return refresh_default_scanout(memory, owner_process_id);
        }
        if (read.state != SurfaceStore::CpuReadResult::State::Completed)
            return false;
        last_scanout_publication_ = publication;
        last_scanout_generation_ = generation;
        last_scanout_gpu_generation_ = read.gpu_generation;
        pixels = std::move(read.pixels);
    }
    if (pixels.empty() || pixels == last_scanout_pixels_)
        return false;

    // A freshly allocated CoreSurface is zero-filled.  Do not replace the
    // display's opaque-black power-on frame until the firmware has actually
    // rendered something into the LCD backing store.
    if (std::none_of(pixels.begin(), pixels.end(),
            [](std::uint32_t pixel) { return pixel != 0; })) {
        last_scanout_pixels_ = pixels;
        return false;
    }

    std::optional<ApplicationDisplay> profile;
    if (shared_state_) {
        std::lock_guard lock { shared_state_->mach_mutex };
        const auto process = shared_state_->processes.find(owner_process_id);
        if (process != shared_state_->processes.end() &&
            process->second.application_display.kind !=
                ApplicationDisplayMode::Native) {
            profile = process->second.application_display;
        }
    }
    auto display_pixels = pixels;
    if (profile) {
        display_pixels = compose_application_display_pixels(*profile,
            { backing->width, backing->height }, display_->geometry(),
            display_pixels);
        if (display_pixels.empty())
            return false;
    }
    last_scanout_pixels_ = pixels;
    display_->replace_pixels(std::move(display_pixels), owner_process_id);
    display_->present(owner_process_id);
    return true;
} catch (...) {
    discard_default_scanout();
    return false;
}

} // namespace ilemu
