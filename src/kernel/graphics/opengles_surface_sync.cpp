// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "kernel/opengles_hle.hpp"

#include "foundation/address_space.hpp"
#include "graphics/surface_cpu_readback.hpp"
#include "graphics/surface_store.hpp"

#include <stdexcept>
#include <utility>

namespace ilemu {

struct OpenGlesHle::PixmapRead {
    SurfaceStore::Backing backing;
    AddressSpace::MappingObservation mapping;
    std::uint64_t owner { };
    std::size_t bytes { };
    std::shared_ptr<SyncLease> lease;
    std::shared_ptr<GlesValueResult<SurfaceCpuReadback>> host;
};

void OpenGlesHle::create_pixmap_surface(UserlandHleCall& call, std::uint32_t id)
try {
    const auto backing = surface_store_->find(id);
    if (!backing) {
        publish_created_surface(call, std::nullopt);
        return;
    }
    constexpr std::size_t limit = 64U * 1024U * 1024U;
    const auto count =
        static_cast<std::uint64_t>(backing->width) * backing->height;
    if (!count || count > limit / sizeof(std::uint32_t)) {
        publish_created_surface(call, std::nullopt);
        return;
    }
    auto read = surface_store_->try_read_argb(call.memory(), id);
    if (read.state == SurfaceStore::CpuReadResult::State::Completed) {
        SurfaceState state;
        state.backing_identifier = id;
        state.width = backing->width;
        state.height = backing->height;
        state.pixels = std::move(read.pixels);
        if (const auto surface = surface_store_->host_surface(id))
            state.backing_cpu_generation = surface->cpu_generation();
        publish_created_surface(call, std::move(state));
        return;
    }
    if (read.state != SurfaceStore::CpuReadResult::State::PendingGpu) {
        publish_created_surface(call, std::nullopt);
        return;
    }
    auto pending = std::make_shared<PixmapRead>();
    pending->backing = *backing;
    pending->mapping =
        call.memory().observe_mapping(backing->base, backing->allocation_size);
    pending->owner = renderer_owner_;
    pending->bytes = static_cast<std::size_t>(count) * sizeof(std::uint32_t);
    pending->host = std::make_shared<GlesValueResult<SurfaceCpuReadback>>();
    pending->host->value.renderer = renderer_;
    pending->host->value.surface = surface_store_->host_surface(id);
    pending->lease = program_wait(call);
    await_pixmap_read(call, std::move(pending));
} catch (...) {
    publish_created_surface(call, std::nullopt);
}

void OpenGlesHle::await_pixmap_read(
    UserlandHleCall& call, std::shared_ptr<PixmapRead> pending)
try {
    const auto& expected = pending->backing;
    const auto backing = surface_store_->find(expected.id);
    const auto host = pending->host;
    if (renderer_owner_ != pending->owner || !pending->mapping.valid() ||
        !backing ||
        backing->provenance.publication_sequence !=
            expected.provenance.publication_sequence ||
        backing->base != expected.base ||
        backing->allocation_size != expected.allocation_size ||
        backing->width != expected.width ||
        backing->height != expected.height ||
        backing->bytes_per_row != expected.bytes_per_row ||
        backing->pixel_format != expected.pixel_format ||
        surface_store_->host_surface(expected.id) != host->value.surface ||
        !call.memory().accessible(
            expected.base, expected.allocation_size, MemoryPermission::Read))
        throw std::runtime_error { "pixmap source was replaced" };
    if (!host->completion) {
        // Only immutable host ownership crosses the queue. The mapping,
        // backing identity, EGL state and continuation remain on the core.
        host->completion = enqueue_gles_host_work(pending->bytes,
            [host](DisplayFrame&) { return host->value.capture(); });
        if (!host->completion) {
            call.suspend_until(
                [bytes = pending->bytes] {
                    return gles_host_work_capacity(bytes);
                },
                [this, pending](UserlandHleCall& resumed) {
                    await_pixmap_read(resumed, pending);
                });
            return;
        }
        pending->lease->result = host->completion;
    }
    if (!host->completion->ready()) {
        call.suspend_until(
            [result = host->completion] { return result->ready(); },
            [this, pending](UserlandHleCall& resumed) {
                await_pixmap_read(resumed, pending);
            },
            host->completion->sequence());
        return;
    }
    const auto& captured = host->value.snapshot;
    if (!host->completion->succeeded() ||
        captured.frame.width != expected.width ||
        captured.frame.height != expected.height ||
        captured.frame.pixels.size() != pending->bytes / sizeof(std::uint32_t))
        throw std::runtime_error { "pixmap capture failed" };
    std::vector<std::uint32_t> pixels;
    // Publish the cache before moving its captured pixels. Only an unchanged
    // CPU/GPU pair can accept it; later GPU writes keep their dirty state.
    if (host->value.surface->commit_cpu_readback(captured)) {
        host->value.renderer->acknowledge_readback(
            host->value.surface->key(), host->value.stamp);
        auto merged = surface_store_->try_read_argb(call.memory(), expected.id);
        if (merged.state == SurfaceStore::CpuReadResult::State::Failed)
            throw std::runtime_error { "pixmap guest import failed" };
        if (merged.state == SurfaceStore::CpuReadResult::State::Completed)
            pixels = std::move(merged.pixels);
    }
    if (pixels.empty())
        pixels = std::move(host->value.snapshot.frame.pixels);
    SurfaceState state;
    state.backing_identifier = expected.id;
    state.width = expected.width;
    state.height = expected.height;
    state.pixels = std::move(pixels);
    // Leave refresh conservative: a later GPU writer or guest CPU edit must
    // not be hidden by sampling a newer watermark after this captured frame.
    pending->lease->consume();
    publish_created_surface(call, std::move(state));
} catch (...) {
    pending->lease->consume();
    publish_created_surface(call, std::nullopt);
}

} // namespace ilemu
