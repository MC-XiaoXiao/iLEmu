// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "kernel/mbx2d_hle.hpp"

#include "foundation/cpu.hpp"
#include "graphics/display.hpp"
#include "surface_guest_import.hpp"

#include <stdexcept>
#include <utility>

namespace ilemu {
namespace {
    void fail_import(UserlandHleCall& call,
        const std::shared_ptr<GraphicsCallLease>& lease)
    {
        lease->cancelled = true;
        // Observer failure after admission does not retire in-flight work.
        // Normal core polling releases the canceled gate after actual ready.
        if (lease->ready())
            lease->consume();
        call.set_return(mbx2d_abi::failure);
    }
}
struct Mbx2dHle::FlushPlan {
    std::uint64_t owner { };
    std::uint32_t array { }, count { };
    AddressSpace::MappingObservation mapping;
    struct Entry {
        std::uint32_t handle { }, id { }, base { }, size { };
        std::uint64_t publication { };
        std::shared_ptr<HostSurface> surface;
        AddressSpace::MappingObservation mapping;
    };
    // Retained input metadata shares the existing preparation budget.
    std::shared_ptr<void> credit;
    std::vector<Entry> entries;
    std::size_t next { };
    bool prepared { };
};

void Mbx2dHle::flush_surfaces(UserlandHleCall& call)
try {
    const auto count = call.argument(1);
    if (!count) {
        call.set_return(mbx2d_abi::success);
        return;
    }
    const auto array = call.argument(0);
    if (!array || count > mbx2d_abi::maximum_flush_surface_count) {
        call.set_return(mbx2d_abi::failure);
        return;
    }
    auto plan = std::make_shared<FlushPlan>();
    plan->owner = renderer_owner_;
    plan->array = array;
    plan->count = count;
    plan->mapping = call.memory().observe_mapping(
        array, static_cast<std::size_t>(count) * sizeof(std::uint32_t));
    auto lease = std::make_shared<GraphicsCallLease>();
    lease->owner = call.cpu().processor_id();
    sync_lease_ = lease;
    await_flush_surfaces(call, std::move(lease), std::move(plan));
} catch (...) {
    call.set_return(mbx2d_abi::failure);
}

void Mbx2dHle::await_flush_surfaces(UserlandHleCall& call,
    std::shared_ptr<GraphicsCallLease> lease, std::shared_ptr<FlushPlan> plan)
try {
    if (plan->owner != renderer_owner_ || lease->cancelled || lease->consumed)
        throw std::runtime_error { "stale MBX import" };
    if (!plan->prepared) {
        const auto bytes =
            static_cast<std::size_t>(plan->count) * sizeof(std::uint32_t);
        if (!plan->mapping.valid() ||
            !call.memory().accessible(plan->array, bytes, MemoryPermission::Read))
            throw std::runtime_error { "MBX surface list was replaced" };
        const auto metadata_bytes =
            static_cast<std::size_t>(plan->count) * sizeof(FlushPlan::Entry);
        plan->credit = reserve_gles_host_preparation(metadata_bytes);
        if (!plan->credit) {
            call.suspend_until(
                [metadata_bytes] {
                    return gles_host_preparation_capacity(metadata_bytes);
                },
                [this, lease, plan](UserlandHleCall& resumed) {
                    await_flush_surfaces(resumed, lease, plan);
                });
            return;
        }
        plan->entries.reserve(plan->count);
        for (std::uint32_t index = 0; index < plan->count; ++index) {
            const auto address = plan->array +
                                 index * static_cast<std::uint32_t>(
                                             sizeof(std::uint32_t));
            const auto handle = call.memory().read32(address);
            const auto found = handle ? surfaces_.find(*handle) : surfaces_.end();
            if (found == surfaces_.end())
                throw std::runtime_error { "missing MBX list surface" };
            FlushPlan::Entry entry;
            entry.handle = *handle;
            entry.id = found->second.core_surface_id;
            if (entry.id != 0) {
                const auto backing = surface_store_->find(entry.id);
                if (!backing)
                    throw std::runtime_error { "missing MBX list backing" };
                entry.base = backing->base;
                entry.size = backing->allocation_size;
                entry.publication = backing->provenance.publication_sequence;
                entry.mapping = call.memory().observe_mapping(
                    entry.base, entry.size);
                entry.surface = surface_store_->host_surface(entry.id);
            }
            plan->entries.push_back(std::move(entry));
        }
        plan->prepared = true;
    }
    // The existing per-store call gate prevents MBX handle/context mutation
    // while this plan is active. Owner incarnation also excludes reset/fork
    // reuse. No guest pointer or map-node address is retained across a yield.
    while (plan->next < plan->entries.size()) {
        const auto& entry = plan->entries[plan->next];
        const auto found = surfaces_.find(entry.handle);
        if (found == surfaces_.end())
            throw std::runtime_error { "missing MBX import surface" };
        if (found->second.client_backing)
            found->second.client_host_source_dirty = true;
        const auto id = found->second.core_surface_id;
        if (id != entry.id)
            throw std::runtime_error { "MBX import handle was replaced" };
        if (id == 0) {
            ++plan->next;
            continue;
        }
        const auto backing = surface_store_->find(id);
        if (!backing || !entry.mapping.valid() || backing->base != entry.base ||
            backing->allocation_size != entry.size ||
            backing->provenance.publication_sequence != entry.publication ||
            surface_store_->host_surface(id) != entry.surface ||
            !call.memory().accessible(
                entry.base, entry.size, MemoryPermission::Read))
            throw std::runtime_error { "MBX import backing was replaced" };
        const auto imported =
            surface_store_->try_synchronize_from_guest(call.memory(), id);
        if (imported == SurfaceStore::CpuImportResult::Failed)
            throw std::runtime_error { "MBX surface import failed" };
        if (imported == SurfaceStore::CpuImportResult::Completed) {
            ++plan->next;
            continue;
        }
        SurfaceGuestImport::run(call, surface_store_, id,
            [this, lease, plan](UserlandHleCall& resumed, bool succeeded) {
                try {
                    if (!succeeded) {
                        fail_import(resumed, lease);
                        return;
                    }
                    ++plan->next;
                    // Keep even an immediately completed import off a recursive
                    // list walk. This is a core continuation, not a GPU completion.
                    resumed.suspend_until([] { return true; },
                        [this, lease, plan](UserlandHleCall& ready) {
                            await_flush_surfaces(ready, lease, plan);
                        });
                } catch (...) {
                    fail_import(resumed, lease);
                }
            },
            lease);
        return;
    }
    lease->consume();
    // Unified-memory visibility only: no encoder finish or scanout is added.
    call.set_return(mbx2d_abi::success);
} catch (...) {
    fail_import(call, lease);
}
void Mbx2dHle::import_source(UserlandHleCall& call, bool context_api,
    ResolvedSurface source, ResolvedSurface destination,
    SourceContinuation continuation, bool capture_pixels,
    std::shared_ptr<GraphicsCallLease> lease)
try {
    const auto observe = [&call](const ResolvedSurface& surface) {
        return surface.backing ? call.memory().observe_mapping(
                                     surface.backing->base,
                                     surface.backing->allocation_size)
                               : AddressSpace::MappingObservation { };
    };
    const auto source_mapping = observe(source);
    const auto destination_mapping = observe(destination);
    const auto owner = renderer_owner_;
    const auto display = display_;
    const auto owns_lease = !lease;
    if (!lease) {
        lease = std::make_shared<GraphicsCallLease>();
        lease->owner = call.cpu().processor_id();
        sync_lease_ = lease;
    }
    const auto id = source.core_surface_id;
    try {
        auto completed =
            [this, context_api, owner, display, capture_pixels,
                source = std::move(source),
                destination = std::move(destination), source_mapping,
                destination_mapping, lease, owns_lease,
                continuation = std::move(continuation)](
                UserlandHleCall& resumed, bool succeeded,
                std::vector<std::uint32_t> pixels) mutable {
                try {
                    const auto* state = select_state(resumed, context_api);
                    const auto live = [](const ResolvedSurface& expected,
                                          const std::optional<ResolvedSurface>& current,
                                          const AddressSpace::MappingObservation& mapping) {
                        if (!current ||
                            expected.surface_handle != current->surface_handle ||
                            expected.core_surface_id != current->core_surface_id ||
                            expected.host_surface != current->host_surface ||
                            expected.width != current->width ||
                            expected.height != current->height ||
                            expected.framebuffer != current->framebuffer ||
                            expected.backing.has_value() != current->backing.has_value())
                            return false;
                        if (!expected.backing)
                            return true;
                        const auto& a = *expected.backing;
                        const auto& b = *current->backing;
                        return mapping.valid() && a.base == b.base &&
                               a.allocation_size == b.allocation_size &&
                               a.bytes_per_row == b.bytes_per_row &&
                               a.pixel_format == b.pixel_format &&
                               a.provenance.publication_sequence ==
                                   b.provenance.publication_sequence;
                    };
                    // The existing MBX gate preserves both bindings/state.
                    // Mapping and publication checks additionally exclude
                    // unmap/remap and shared backing replacement during wait.
                    if (!succeeded || owner != renderer_owner_ || !state ||
                        lease->cancelled || display != display_ ||
                        !live(source, resolve(state->source), source_mapping) ||
                        !live(destination, resolve(state->destination),
                            destination_mapping)) {
                        fail_import(resumed, lease);
                        return;
                    }
                    if (capture_pixels) {
                        if (pixels.size() !=
                            static_cast<std::uint64_t>(source.width) * source.height)
                            throw std::runtime_error { "invalid MBX source image" };
                        source.cpu_frame = std::make_shared<DisplayFrame>(
                            DisplayFrame { source.width, source.height, 0,
                                std::move(pixels) });
                    }
                    continuation(resumed, source, destination);
                    if (owns_lease || !resumed.has_pending_completion())
                        lease->consume();
                } catch (...) {
                    fail_import(resumed, lease);
                }
            };
        if (capture_pixels) {
            SurfaceGuestImport::read(call, surface_store_, id,
                [completed = std::move(completed)](UserlandHleCall& resumed,
                    std::optional<std::vector<std::uint32_t>> pixels) mutable {
                    const auto succeeded = pixels.has_value();
                    completed(resumed, succeeded,
                        succeeded ? std::move(*pixels)
                                  : std::vector<std::uint32_t> { });
                },
                lease);
        } else {
            SurfaceGuestImport::run(call, surface_store_, id,
                [completed = std::move(completed)](
                    UserlandHleCall& resumed, bool succeeded) mutable {
                    completed(resumed, succeeded, { });
                },
                lease);
        }
    } catch (...) {
        fail_import(call, lease);
    }
} catch (...) {
    call.set_return(mbx2d_abi::failure);
}

} // namespace ilemu
