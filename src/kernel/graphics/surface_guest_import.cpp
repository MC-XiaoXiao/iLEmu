// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "surface_guest_import.hpp"

#include "kernel/graphics_call_lease.hpp"

#include "foundation/address_space.hpp"
#include "foundation/userland_hle.hpp"
#include "graphics/gles_program_renderer.hpp"
#include "graphics/surface_cpu_readback.hpp"
#include "graphics/surface_store.hpp"

#include <stdexcept>
#include <utility>

namespace ilemu {
struct SurfaceGuestImport::Pending {
    std::shared_ptr<SurfaceStore> store;
    SurfaceStore::Backing backing;
    AddressSpace::MappingObservation mapping;
    std::shared_ptr<HostSurface> surface;
    std::shared_ptr<GlesValueResult<SurfaceCpuReadback>> host;
    Completion completion;
    std::shared_ptr<GraphicsCallLease> lease;
    std::size_t bytes { };
    bool consumed { };

    void finish(UserlandHleCall& call, bool succeeded)
    {
        if (std::exchange(consumed, true))
            return;
        auto callback = std::move(completion);
        callback(call, succeeded);
    }
};

void SurfaceGuestImport::run(UserlandHleCall& call,
    std::shared_ptr<SurfaceStore> store, std::uint32_t id, Completion completion,
    std::shared_ptr<GraphicsCallLease> lease)
{
    auto pending = std::make_shared<Pending>();
    pending->store = std::move(store);
    pending->completion = std::move(completion);
    pending->lease = std::move(lease);
    try {
        const auto backing = pending->store->find(id);
        if (!backing)
            throw std::runtime_error { "missing guest import backing" };
        pending->backing = *backing;
        pending->mapping = call.memory().observe_mapping(
            backing->base, backing->allocation_size);
        pending->surface = pending->store->host_surface(id);
        resume(call, pending);
    } catch (...) {
        pending->finish(call, false);
    }
}

void SurfaceGuestImport::resume(
    UserlandHleCall& call, std::shared_ptr<Pending> pending)
try {
    if (pending->consumed)
        return;
    const auto& expected = pending->backing;
    const auto backing = pending->store->find(expected.id);
    if (!pending->mapping.valid() || !backing ||
        backing->provenance.publication_sequence !=
            expected.provenance.publication_sequence ||
        backing->base != expected.base ||
        backing->allocation_size != expected.allocation_size ||
        backing->width != expected.width || backing->height != expected.height ||
        backing->bytes_per_row != expected.bytes_per_row ||
        backing->pixel_format != expected.pixel_format ||
        pending->store->host_surface(expected.id) != pending->surface ||
        !call.memory().accessible(
            expected.base, expected.allocation_size, MemoryPermission::Read))
        throw std::runtime_error { "guest import backing was replaced" };
    if (pending->host && pending->host->completion &&
        pending->host->completion->ready()) {
        const auto& read = pending->host->value;
        if (!pending->host->completion->succeeded())
            throw std::runtime_error { "guest import capture failed" };
        // This direction updates only the host cache. Unlock must not publish
        // GPU pixels into guest memory. A concurrent newer CPU/GPU pair rejects
        // this snapshot; the atomic import below decides if another is needed.
        if (read.surface->commit_cpu_readback(read.snapshot))
            read.renderer->acknowledge_readback(read.surface->key(), read.stamp);
        if (pending->lease)
            pending->lease->result.reset();
        pending->host.reset();
    }
    if (!pending->host) {
        const auto imported = pending->store->try_synchronize_from_guest(
            call.memory(), expected.id);
        if (imported != SurfaceStore::CpuImportResult::PendingGpu) {
            pending->finish(
                call, imported == SurfaceStore::CpuImportResult::Completed);
            return;
        }
        // The readback budget limits only pending GPU payloads. CPU-only
        // imports keep the existing SurfaceStore allocation-size limits.
        const auto count =
            static_cast<std::uint64_t>(expected.width) * expected.height;
        constexpr std::size_t limit = 64U * 1024U * 1024U;
        if (count > limit / sizeof(std::uint32_t))
            throw std::length_error { "guest import exceeds 64 MiB" };
        pending->bytes = static_cast<std::size_t>(count) * sizeof(std::uint32_t);
        pending->host = std::make_shared<GlesValueResult<SurfaceCpuReadback>>();
        pending->host->value.renderer = shared_gles_renderer();
        pending->host->value.surface = pending->surface;
    }
    const auto host = pending->host;
    if (!host->completion) {
        host->completion = enqueue_gles_host_work(pending->bytes,
            [host](DisplayFrame&) { return host->value.capture(); });
        if (!host->completion) {
            call.suspend_until(
                [bytes = pending->bytes] {
                    return gles_host_work_capacity(bytes);
                },
                [pending](UserlandHleCall& resumed) { resume(resumed, pending); });
            return;
        }
    }
    const auto ticket = host->completion;
    if (pending->lease)
        pending->lease->result = ticket;
    call.suspend_until([ticket] { return ticket->ready(); },
        [pending](UserlandHleCall& resumed) { resume(resumed, pending); },
        ticket->sequence);
} catch (...) {
    pending->finish(call, false);
}
} // namespace ilemu
