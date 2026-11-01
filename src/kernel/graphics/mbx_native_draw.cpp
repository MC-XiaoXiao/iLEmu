// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "kernel/mbx2d_hle.hpp"

#include "foundation/cpu.hpp"
#include "foundation/performance.hpp"
#include "graphics/gles_renderer.hpp"

#include <chrono>
#include <stdexcept>
#include <utility>

namespace ilemu {
struct Mbx2dHle::NativeDraw {
    struct Work {
        std::shared_ptr<void> credit;
        std::shared_ptr<SurfaceTransportLease> transport, source_transport;
        std::shared_ptr<GlesRendererOwner> native_lifetime;
        std::shared_ptr<GlesRenderer> renderer;
        std::shared_ptr<CommandEncoder> encoder;
        std::shared_ptr<HostSurface> target, source;
        HostRectangle rectangle { }, source_rectangle { };
        std::uint32_t color { };
        HostCompositeMode mode { };
        std::uint8_t alpha { };
        HostRotation rotation { HostRotation::Identity };

        bool execute() const
        {
            if (!source)
                return encoder->fill(target, rectangle, color, mode, alpha);
            const auto started = std::chrono::steady_clock::now();
            const auto encoded = encoder->copy(source, target, source_rectangle,
                rectangle, mode, alpha, HostFilter::Nearest, rotation);
            performance_counters().record_diagnostic_graphics_hle(
                PerfDiagnosticGraphicsHleKind::HostCopyEncode,
                static_cast<std::uint64_t>(
                    std::chrono::duration_cast<std::chrono::nanoseconds>(
                        std::chrono::steady_clock::now() - started).count()));
            return encoded;
        }
    };
    std::shared_ptr<Work> work;
    ResolvedSurface destination;
    std::optional<ResolvedSurface> source;
    AddressSpace::MappingObservation mapping, source_mapping;
    std::shared_ptr<GraphicsCallLease> lease;
    std::shared_ptr<const GlesWorkCompletion> previous_writer;
    std::uint64_t owner { };
    bool context_api { };
};

std::shared_ptr<Mbx2dHle::NativeDraw> Mbx2dHle::prepare_native_draw(
    UserlandHleCall& call, bool context_api, ResolvedSurface destination,
    std::optional<ResolvedSurface> source)
{
    auto pending = std::make_shared<NativeDraw>();
    pending->destination = std::move(destination);
    const auto& target = pending->destination;
    pending->mapping = call.memory().observe_mapping(
        target.backing->base, target.backing->allocation_size);
    pending->owner = renderer_owner_;
    pending->context_api = context_api;
    pending->work = std::make_shared<NativeDraw::Work>();
    const auto work = pending->work;
    work->renderer = host_graphics_;
    work->native_lifetime = renderer_lifetime_;
    work->encoder = command_encoder_;
    work->target = target.host_surface;
    work->transport =
        surface_store_->acquire_transport_lease(target.core_surface_id);
    if (!work->transport)
        throw std::runtime_error { "missing MBX native target owner" };
    if (source) {
        pending->source = std::move(source);
        const auto& input = *pending->source;
        pending->source_mapping = call.memory().observe_mapping(
            input.backing->base, input.backing->allocation_size);
        work->source = input.host_surface;
        if (input.core_surface_id) {
            work->source_transport =
                surface_store_->acquire_transport_lease(input.core_surface_id);
            if (!work->source_transport)
                throw std::runtime_error { "missing MBX native source owner" };
        }
    }
    pending->lease = std::make_shared<GraphicsCallLease>();
    pending->lease->owner = call.cpu().processor_id();
    sync_lease_ = pending->lease;
    return pending;
}

bool Mbx2dHle::native_draw_live(
    UserlandHleCall& call, const NativeDraw& pending)
{
    const auto* bindings = select_state(call, pending.context_api);
    if (pending.owner != renderer_owner_ || pending.lease->cancelled ||
        pending.lease->consumed || !bindings ||
        !surface_binding_live(pending.destination, resolve(bindings->destination),
            pending.mapping))
        return false;
    if (!pending.source)
        return true;
    const auto& source = *pending.source;
    if (!surface_binding_live(source, resolve(bindings->source),
            pending.source_mapping))
        return false;
    // Raw client inputs resolve to CPU metadata; their frozen native snapshot
    // is owned by this exact MBX namespace rather than a shared transport.
    return source.core_surface_id != 0 ||
           surfaces_.at(source.surface_handle).client_host_source ==
               pending.work->source;
}

void Mbx2dHle::fill_native(UserlandHleCall& call, bool context_api,
    ResolvedSurface destination, HostRectangle rectangle,
    std::uint32_t color, HostCompositeMode mode, std::uint8_t alpha)
try {
    auto pending = prepare_native_draw(
        call, context_api, std::move(destination));
    pending->work->rectangle = rectangle;
    pending->work->color = color;
    pending->work->mode = mode;
    pending->work->alpha = alpha;
    await_native_draw(call, std::move(pending));
} catch (...) {
    call.set_return(mbx2d_abi::failure);
}

void Mbx2dHle::copy_native(UserlandHleCall& call, bool context_api,
    const ResolvedSurface& source, const ResolvedSurface& destination,
    HostRectangle source_rectangle, HostRectangle destination_rectangle,
    HostCompositeMode mode, std::uint8_t alpha, HostRotation rotation)
try {
    auto pending = prepare_native_draw(call, context_api, destination, source);
    pending->work->source_rectangle = source_rectangle;
    pending->work->rectangle = destination_rectangle;
    pending->work->mode = mode;
    pending->work->alpha = alpha;
    pending->work->rotation = rotation;
    await_native_draw(call, std::move(pending));
} catch (...) {
    call.set_return(mbx2d_abi::failure);
}

void Mbx2dHle::await_native_draw(
    UserlandHleCall& call, std::shared_ptr<NativeDraw> pending)
try {
    if (!native_draw_live(call, *pending))
        throw std::runtime_error { "stale MBX native binding" };
    const auto gate = pending->lease;
    const auto work = pending->work;
    if (!gate->result) {
        constexpr auto bytes = sizeof(NativeDraw) + sizeof(NativeDraw::Work) +
                               16U * sizeof(void*);
        if (!work->credit) {
            work->credit = reserve_gles_host_preparation(bytes);
            if (!work->credit) {
                call.suspend_until(
                    [] { return gles_host_preparation_capacity(bytes); },
                    [this, pending](UserlandHleCall& resumed) {
                        await_native_draw(resumed, pending);
                    });
                return;
            }
        }
        auto dependency = work->target->writer();
        if (dependency && dependency->ready() && !dependency->succeeded())
            throw std::runtime_error { "failed MBX native writer" };
        pending->previous_writer = dependency;
        if (!dependency || dependency->ready())
            dependency = work->target->reader();
        if ((!dependency || dependency->ready()) && work->source) {
            dependency = work->source->writer();
            if (dependency && dependency->ready() && !dependency->succeeded())
                throw std::runtime_error { "failed MBX native source writer" };
        }
        if (dependency && !dependency->ready()) {
            call.suspend_until([dependency] { return dependency->ready(); },
                [this, pending](UserlandHleCall& resumed) {
                    await_native_draw(resumed, pending);
                }, dependency->sequence());
            return;
        }
        gate->result = enqueue_gles_surface_write(work->target,
            [work](DisplayFrame&) { return work->execute(); });
        if (!gate->result) {
            call.suspend_until([] { return gles_host_work_capacity(0); },
                [this, pending](UserlandHleCall& resumed) {
                    await_native_draw(resumed, pending);
                });
            return;
        }
        // Serial core admission and reader registration precede any later
        // guest writer. The existing FIFO orders all reads of this image.
        if (work->source)
            work->source->record_read(gate->result->completion());
    }
    const auto result = gate->result;
    call.suspend_until([result] { return result->ready(); },
        [this, pending](UserlandHleCall& resumed) {
            const auto live = native_draw_live(resumed, *pending);
            const auto lease = pending->lease;
            if (!live) {
                lease->consume();
                resumed.set_return(mbx2d_abi::failure);
                return;
            }
            const auto succeeded = lease->result->succeeded();
            if (!succeeded) {
                // Keep the original software fallback, with the original ABI
                // and store gate. The failed native command is never retried.
                pending->work->target->abandon_write(
                    lease->result->completion(), pending->previous_writer);
                lease->consume();
                if (pending->source)
                    execute_blit_copy(resumed, pending->context_api,
                        *pending->source, pending->destination, { }, true);
                else
                    blit_color(resumed, pending->context_api, true);
                return;
            }
            lease->consume();
            resumed.set_return(mbx2d_abi::success);
        }, result->sequence());
} catch (...) {
    pending->lease->cancelled = true;
    if (pending->lease->ready())
        pending->lease->consume();
    call.set_return(mbx2d_abi::failure);
}
} // namespace ilemu
