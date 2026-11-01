// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "kernel/core_surface_hle.hpp"

#include "foundation/address_space.hpp"
#include "foundation/userland_hle.hpp"
#include "graphics/core_surface_abi.hpp"
#include "graphics/gles_program_renderer.hpp"
#include "graphics/surface_cpu_readback.hpp"
#include "graphics/surface_store.hpp"

namespace ilemu {

// This record stays on the core. Only its host-only value crosses the worker
// queue; no Buffer, AddressSpace, Cpu or temporary call reference does.
struct CoreSurfaceHle::CpuAccess {
    std::uint32_t client { };
    bool releasing { };
    std::shared_ptr<const BufferLifetime> identity;
    SurfaceStore::Backing backing;
    AddressSpace::MappingObservation mapping, client_mapping, seed_mapping;
    std::size_t bytes { };
    std::shared_ptr<GlesValueResult<SurfaceCpuReadback>> result;
};

void CoreSurfaceHle::complete_buffer_lock(UserlandHleCall& call, Buffer& buffer,
    bool synchronized, bool seed_mapping_live)
{
    if (synchronized)
        buffer.lock_options.push_back(call.argument(1));
    const auto& abi = surface_transport::for_kind(buffer.transport);
    if (synchronized && abi.lock_seed_output && call.argument(2) != 0 &&
        seed_mapping_live)
        static_cast<void>(call.memory().write32(call.argument(2), buffer.seed));
    call.set_return(synchronized ? 0U : 1U);
}

void CoreSurfaceHle::release_buffer(UserlandHleCall& call, Buffer& buffer)
{
    if (buffer.references == 1U && !buffer.owns_memory) {
        // A caller-owned pixel buffer remains live after its wrapper's last
        // release. Publish GPU pixels before releasing that wrapper reference.
        synchronize_buffer(call, buffer, true);
        return;
    }
    complete_buffer_release(call, buffer);
}

void CoreSurfaceHle::complete_buffer_release(
    UserlandHleCall& call, Buffer& buffer, bool client_mapping_live)
{
    const auto& abi = surface_transport::for_kind(buffer.transport);
    if (buffer.references != 0)
        --buffer.references;
    if (client_mapping_live) {
        try {
            static_cast<void>(call.memory().write32(
                buffer.client + abi.reference_count_offset, buffer.references));
        } catch (...) {
            // A guest wrapper write failure does not undo its logical release.
        }
    }
    if (buffer.references == 0) {
        const auto client = buffer.client;
        const auto id = buffer.id;
        const auto mapping_base = buffer.imported_mapping_base;
        const auto mapping_size = buffer.imported_mapping_size;
        const auto mapping_token = buffer.imported_mapping_lease_token;
        const auto indexed = clients_by_id_.find({ id, buffer.transport });
        if (indexed != clients_by_id_.end() && indexed->second == client)
            clients_by_id_.erase(indexed);
        buffers_.erase(client);
        // The shared publication's pre-admitted renderer owner posts bounded
        // worker retirement; removing this store reference never waits for GPU.
        surfaces_->release(id);
        release_imported_mapping(
            call.memory(), mapping_base, mapping_size, mapping_token);
        if (client_mapping_live)
            recycle_client_buffer(client, abi);
    }
    call.set_return(0U);
}

void CoreSurfaceHle::synchronize_buffer(
    UserlandHleCall& call, Buffer& buffer, bool releasing)
{
    const auto client = buffer.client;
    const auto identity = buffer.identity;
    const auto fail = [&] {
        const auto found = buffers_.find(client);
        if (releasing && found != buffers_.end() &&
            found->second.identity == identity)
            complete_buffer_release(call, found->second);
        call.set_return(1U);
    };
    try {
        const auto options =
            releasing ? core_surface_abi::lock_read_only : call.argument(1);
        const auto required = surfaces_->cpu_sync_required(buffer.id,
            SurfaceStore::CpuSynchronizationOptions {
                .avoid_sync =
                    (options & core_surface_abi::lock_avoid_sync) != 0,
                .read_only = (options & core_surface_abi::lock_read_only) != 0,
            });
        if (!required || !*required) {
            if (releasing) {
                complete_buffer_release(call, buffer);
                if (!required)
                    call.set_return(1U);
            } else {
                complete_buffer_lock(call, buffer, required.has_value());
            }
            return;
        }
        const auto backing = surfaces_->find(buffer.id);
        if (!backing) {
            fail();
            return;
        }
        // Retained pixel results share the existing 64 MiB queue budget.
        // Core copyout is serial: merge/row-decode/encode scratch is separately
        // bounded by two complete frames (128 MiB), never retained per waiter.
        constexpr std::size_t limit = 64U * 1024U * 1024U;
        const auto pixels =
            static_cast<std::uint64_t>(backing->width) * backing->height;
        if (pixels > limit / sizeof(std::uint32_t)) {
            fail();
            return;
        }
        auto pending = std::make_shared<CpuAccess>();
        pending->client = buffer.client;
        pending->releasing = releasing;
        pending->identity = buffer.identity;
        pending->backing = *backing;
        pending->bytes =
            static_cast<std::size_t>(pixels) * sizeof(std::uint32_t);
        pending->mapping = call.memory().observe_mapping(
            backing->base, backing->allocation_size);
        const auto& abi = surface_transport::for_kind(buffer.transport);
        pending->client_mapping = call.memory().observe_mapping(
            buffer.client + abi.reference_count_offset, sizeof(std::uint32_t));
        if (!pending->mapping.valid() || !pending->client_mapping.valid()) {
            fail();
            return;
        }
        if (!releasing &&
            surface_transport::for_kind(buffer.transport).lock_seed_output &&
            call.argument(2) != 0)
            pending->seed_mapping = call.memory().observe_mapping(
                call.argument(2), sizeof(std::uint32_t));
        pending->result =
            std::make_shared<GlesValueResult<SurfaceCpuReadback>>();
        pending->result->value.renderer = shared_gles_renderer();
        pending->result->value.surface = surfaces_->host_surface(buffer.id);
        await_cpu_access(call, std::move(pending));
    } catch (...) {
        fail();
    }
}

void CoreSurfaceHle::await_cpu_access(
    UserlandHleCall& call, std::shared_ptr<CpuAccess> pending)
{
    const auto buffer = buffers_.find(pending->client);
    if (buffer == buffers_.end() ||
        buffer->second.identity != pending->identity ||
        !pending->mapping.valid() || !pending->client_mapping.valid()) {
        complete_cpu_access(call, pending, false);
        return;
    }
    auto& host = pending->result;
    if (!host->completion) {
        try {
            host->completion = enqueue_gles_host_work(pending->bytes,
                [host](DisplayFrame&) { return host->value.capture(); });
        } catch (...) {
            complete_cpu_access(call, pending, false);
            return;
        }
        if (!host->completion) {
            const auto bytes = pending->bytes;
            call.suspend_until(
                [bytes] { return gles_host_work_capacity(bytes); },
                [this, pending](UserlandHleCall& resumed) {
                    await_cpu_access(resumed, pending);
                });
            return;
        }
    }
    const auto result = host->completion;
    call.suspend_until([result] { return result->ready(); },
        [this, pending](UserlandHleCall& resumed) {
            complete_cpu_access(
                resumed, pending, pending->result->completion->succeeded());
        },
        result->sequence());
}

void CoreSurfaceHle::complete_cpu_access(UserlandHleCall& call,
    const std::shared_ptr<CpuAccess>& pending, bool host_succeeded)
{
    const auto found = buffers_.find(pending->client);
    if (found == buffers_.end() ||
        found->second.identity != pending->identity) {
        call.set_return(1U);
        return;
    }
    const auto client_live = pending->client_mapping.valid();
    bool synchronized = false;
    if (host_succeeded && client_live && pending->mapping.valid()) {
        try {
            synchronized = surfaces_->complete_cpu_readback(
                call.memory(), pending->backing, pending->result->value);
        } catch (...) {
            // Preserve logical Release even if capture/copyout failed. Failed
            // native resources stay owned by the worker's quarantine policy.
        }
    }
    try {
        if (pending->releasing) {
            complete_buffer_release(call, found->second, client_live);
            if (!synchronized)
                call.set_return(1U);
        } else {
            complete_buffer_lock(call, found->second, synchronized,
                pending->seed_mapping.valid());
        }
    } catch (...) {
        call.set_return(1U);
    }
}

} // namespace ilemu
