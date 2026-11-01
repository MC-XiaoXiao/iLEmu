// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "kernel/mbx2d_hle.hpp"

#include "foundation/cpu.hpp"
#include "surface_guest_import.hpp"

#include <stdexcept>
#include <utility>

namespace ilemu {
struct Mbx2dHle::CpuDraw {
    // Credit outlives its owned result, including cancellation while parked.
    std::shared_ptr<void> credit;
    std::vector<std::uint32_t> pixels;
    std::optional<ResolvedSurface> source;
    ResolvedSurface destination;
    AddressSpace::MappingObservation source_mapping, destination_mapping;
    std::shared_ptr<DisplayState> display;
    std::shared_ptr<GraphicsCallLease> lease;
    CpuContinuation continuation;
    std::optional<BlendState> blend;
    HostRectangle rectangle { };
    std::optional<std::array<HostPoint, 4>> coverage;
    std::uint64_t owner { };
    std::size_t bytes { };
    bool context_api { };
};

std::shared_ptr<GraphicsCallLease> Mbx2dHle::cpu_draw_lease(
    const std::shared_ptr<CpuDraw>& cpu)
{
    return cpu ? cpu->lease : nullptr;
}

bool Mbx2dHle::surface_binding_live(const ResolvedSurface& expected,
    const std::optional<ResolvedSurface>& current,
    const AddressSpace::MappingObservation& mapping)
{
    if (!current || !mapping.valid() || !expected.backing ||
        !current->backing || expected.surface_handle != current->surface_handle ||
        expected.core_surface_id != current->core_surface_id ||
        (expected.core_surface_id != 0 &&
            expected.host_surface != current->host_surface) ||
        expected.width != current->width || expected.height != current->height)
        return false;
    const auto& a = *expected.backing;
    const auto& b = *current->backing;
    return a.base == b.base && a.allocation_size == b.allocation_size &&
           a.bytes_per_row == b.bytes_per_row && a.pixel_format == b.pixel_format &&
           a.provenance.publication_sequence == b.provenance.publication_sequence;
}

bool Mbx2dHle::cpu_draw_live(
    UserlandHleCall& call, const CpuDraw& cpu, bool check_source) const
{
    if (cpu.owner != renderer_owner_ || cpu.display != display_ ||
        cpu.lease->cancelled || cpu.lease->consumed)
        return false;
    const auto state = cpu.context_api ? contexts_.find(call.argument(0))
                                      : contexts_.end();
    if (cpu.context_api && state == contexts_.end())
        return false;
    const auto& bindings = cpu.context_api ? state->second : state_;
    return surface_binding_live(cpu.destination, resolve(bindings.destination),
               cpu.destination_mapping) &&
           (!check_source || !cpu.source ||
               surface_binding_live(
                   *cpu.source, resolve(bindings.source), cpu.source_mapping));
}

void Mbx2dHle::prepare_cpu_draw(UserlandHleCall& call, bool context_api,
    std::optional<ResolvedSurface> source, ResolvedSurface destination,
    std::size_t bytes, CpuContinuation continuation)
try {
    constexpr std::size_t limit = 64U * 1024U * 1024U;
    if (!bytes || bytes > limit || (source && !source->backing) ||
        !destination.backing)
        throw std::length_error { "MBX CPU output exceeds its budget" };
    auto cpu = std::make_shared<CpuDraw>();
    cpu->source = std::move(source);
    cpu->destination = std::move(destination);
    if (cpu->source)
        cpu->source_mapping = call.memory().observe_mapping(
            cpu->source->backing->base, cpu->source->backing->allocation_size);
    cpu->destination_mapping = call.memory().observe_mapping(
        cpu->destination.backing->base, cpu->destination.backing->allocation_size);
    cpu->owner = renderer_owner_;
    cpu->display = display_;
    cpu->context_api = context_api;
    const auto* state = select_state(call, context_api);
    if (!state)
        throw std::runtime_error { "missing MBX CPU state" };
    if (state->enabled_features.contains(mbx2d_abi::feature_blend))
        cpu->blend = state->blend;
    cpu->bytes = bytes;
    cpu->continuation = std::move(continuation);
    cpu->lease = std::make_shared<GraphicsCallLease>();
    cpu->lease->owner = call.cpu().processor_id();
    sync_lease_ = cpu->lease;
    await_cpu_draw(call, std::move(cpu));
} catch (...) {
    call.set_return(mbx2d_abi::failure);
}

void Mbx2dHle::await_cpu_draw(
    UserlandHleCall& call, std::shared_ptr<CpuDraw> cpu)
try {
    if (!cpu_draw_live(call, *cpu, true))
        throw std::runtime_error { "stale MBX CPU inputs" };
    cpu->credit = reserve_gles_host_preparation(cpu->bytes);
    if (!cpu->credit) {
        call.suspend_until(
            [bytes = cpu->bytes] { return gles_host_preparation_capacity(bytes); },
            [this, cpu](UserlandHleCall& resumed) { await_cpu_draw(resumed, cpu); });
        return;
    }
    auto continuation = std::move(cpu->continuation);
    continuation(call, cpu->source, cpu->destination, cpu);
    if (!call.has_pending_completion())
        cpu->lease->consume();
} catch (...) {
    cpu->lease->cancelled = true;
    if (cpu->lease->ready())
        cpu->lease->consume();
    call.set_return(mbx2d_abi::failure);
}

void Mbx2dHle::write_cpu_draw(UserlandHleCall& call,
    std::shared_ptr<CpuDraw> cpu, HostRectangle rectangle,
    std::vector<std::uint32_t> pixels,
    std::optional<std::array<HostPoint, 4>> coverage)
{
    if (!cpu || pixels.size() > cpu->bytes / sizeof(std::uint32_t)) {
        call.set_return(mbx2d_abi::failure);
        return;
    }
    cpu->rectangle = rectangle;
    cpu->pixels = std::move(pixels);
    cpu->coverage = std::move(coverage);
    // The input is now owned output. No later stage rereads guest source bytes
    // or replays the CPU renderer. Only the original target must remain live.
    cpu->continuation = { };
    await_cpu_write(call, std::move(cpu));
}

void Mbx2dHle::await_cpu_write(UserlandHleCall& call,
    std::shared_ptr<CpuDraw> cpu, std::optional<std::uint64_t> gpu)
try {
    if (!cpu_draw_live(call, *cpu, false))
        throw std::runtime_error { "stale MBX CPU output" };
    const auto& target = cpu->destination;
    // Preparation/readback may have yielded since the call-entry gate. A
    // full CPU replacement needs no old pixels, but must still follow every
    // previously accepted reader/writer of this physical image.
    auto dependency = target.host_surface->writer();
    if (dependency && dependency->ready() && !dependency->succeeded())
        throw std::runtime_error { "failed MBX CPU target writer" };
    if (!dependency || dependency->ready())
        dependency = target.host_surface->reader();
    if (dependency && !dependency->ready()) {
        call.suspend_until([dependency] { return dependency->ready(); },
            [this, cpu](UserlandHleCall& resumed) {
                await_cpu_write(resumed, cpu);
            }, dependency->sequence());
        return;
    }
    const auto r = cpu->rectangle;
    const auto full = r.x == 0 && r.y == 0 && r.width == target.width &&
                      r.height == target.height;
    if ((!full || cpu->blend || cpu->coverage) && !gpu) {
        const auto observed = target.host_surface->gpu_generation();
        const auto required = surface_store_->cpu_sync_required(
            target.core_surface_id, { });
        if (!required)
            throw std::runtime_error { "missing MBX CPU target" };
        if (!*required)
            gpu = observed;
    }
    if ((full && !cpu->blend && !cpu->coverage) || gpu) {
        // Source sampling/transformation is already owned. Only the target
        // read/merge may be retried if another producer advances its boundary.
        std::optional<std::vector<std::uint32_t>> blended;
        bool target_current = true;
        if (cpu->blend || cpu->coverage) {
            std::optional<std::vector<std::uint32_t>> destination;
            {
                auto current = target.host_surface->try_map_current_cpu(
                    false, PerfCpuMapReason::SoftwareFallback);
                target_current = current && current->gpu_generation() == *gpu;
                if (target_current) {
                    destination = read_guest_region(
                        target, r.x, r.y, r.width, r.height, call);
                    if (!destination)
                        throw std::runtime_error { "unreadable MBX blend target" };
                }
            }
            if (destination) {
                blended = cpu->blend
                    ? composite_pixels(*cpu->blend, cpu->pixels,
                          cpu->coverage ? *destination : std::move(*destination))
                    : std::optional { cpu->pixels };
                if (!blended)
                    throw std::runtime_error { "invalid MBX blend output" };
                if (cpu->coverage) {
                    for (std::size_t index = 0; index < blended->size(); ++index) {
                        const auto x = static_cast<std::int64_t>(r.x) +
                                       static_cast<std::int64_t>(index % r.width);
                        const auto y = static_cast<std::int64_t>(r.y) +
                                       static_cast<std::int64_t>(index / r.width);
                        if (!quad_contains(*cpu->coverage,
                                { static_cast<float>(x) + 0.5F,
                                    static_cast<float>(y) + 0.5F }))
                            (*blended)[index] = (*destination)[index];
                    }
                }
            }
        }
        if (target_current) {
            const auto written = try_write_region(target, r.x, r.y, r.width,
                r.height, blended ? *blended : cpu->pixels, call, gpu);
            if (written != CpuWriteState::PendingGpu) {
                // These are completed emulator CPU writes, not newer raw
                // guest edits to preserve over a subsequent GPU generation.
                const auto recorded = written == CpuWriteState::Completed &&
                                      surface_store_->record_cpu_write(
                                          *target.backing, r,
                                          blended ? *blended : cpu->pixels);
                cpu->lease->consume();
                call.set_return(recorded ? mbx2d_abi::success
                                         : mbx2d_abi::failure);
                return;
            }
        }
    }
    // Preserve the legacy partial-write visibility: publish real GPU contents
    // before changing guest rows. A later GPU generation causes a fresh capture,
    // never a blocking map or an old stamp covering a new dependency.
    SurfaceGuestImport::publish(call, surface_store_, target.core_surface_id,
        [this, cpu](UserlandHleCall& resumed, bool succeeded,
            std::uint64_t captured_gpu) {
            if (!succeeded) {
                cpu->lease->cancelled = true;
                if (cpu->lease->ready())
                    cpu->lease->consume();
                resumed.set_return(mbx2d_abi::failure);
                return;
            }
            await_cpu_write(resumed, cpu, captured_gpu);
        },
        cpu->lease);
} catch (...) {
    cpu->lease->cancelled = true;
    if (cpu->lease->ready())
        cpu->lease->consume();
    call.set_return(mbx2d_abi::failure);
}
} // namespace ilemu
