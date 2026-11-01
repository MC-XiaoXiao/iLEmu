// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "kernel/mbx2d_hle.hpp"

#include "foundation/cpu.hpp"
#include "graphics/gles_renderer.hpp"

#include <utility>

namespace ilemu {
namespace {
    struct FinishBackend {
        // The encoder can reference its renderer; destruction order is
        // explicit.
        std::shared_ptr<GlesRenderer> renderer;
        std::shared_ptr<CommandEncoder> encoder;

        bool execute(std::span<const HostSurfaceKey> retired) const
        {
            if (!encoder || !encoder->finish(PerfSubmitReason::MbxFinish))
                return false;
            return renderer && renderer->release(retired);
        }
    };
} // namespace

void Mbx2dHle::service_graphics_completions()
{
    if (sync_lease_ && sync_lease_->cancelled && sync_lease_->ready()) {
        sync_lease_->consume();
        sync_lease_.reset();
    }
}

void Mbx2dHle::cancel_pending_work()
{
    if (sync_lease_) {
        sync_lease_->cancelled = true;
        sync_lease_->complete_core = { };
    }
}

void Mbx2dHle::retire_thread(std::size_t processor)
{
    if (sync_lease_ && sync_lease_->owner == processor)
        sync_lease_->cancelled = true;
}

bool Mbx2dHle::finish_destination_live(const FinishPlan& plan) const
{
    const auto current = resolve(state_.destination);
    if (current.has_value() != plan.has_destination)
        return false;
    if (!current)
        return true;
    if (current->core_surface_id != plan.surface_id ||
        current->width != plan.width || current->height != plan.height ||
        current->framebuffer != plan.framebuffer ||
        (plan.destination && current->host_surface != plan.destination))
        return false;
    if (plan.surface_id != 0) {
        const auto backing = surface_store_->find(plan.surface_id);
        if (!backing ||
            backing->provenance.publication_sequence != plan.publication)
            return false;
    }
    return true;
}

void Mbx2dHle::finish(UserlandHleCall& call, bool terminate)
{
    auto lease = std::make_shared<GraphicsCallLease>();
    lease->owner = call.cpu().processor_id();
    auto plan = std::make_shared<FinishPlan>();
    plan->owner = renderer_owner_;
    const auto destination = resolve(state_.destination);
    plan->has_destination = destination.has_value();
    if (destination) {
        plan->destination = destination->host_surface;
        plan->surface_id = destination->core_surface_id;
        plan->width = destination->width;
        plan->height = destination->height;
        plan->framebuffer = destination->framebuffer;
        if (destination->backing)
            plan->publication =
                destination->backing->provenance.publication_sequence;
    }
    // The private store stays gated while this plan is pending. Cancellation
    // before admission or failure before cleanup returns the owned snapshot.
    lease->complete_core = [this, plan] {
        if (plan->prepared && (!plan->accepted || !plan->retired_completed) &&
            plan->owner == renderer_owner_)
            retired_client_host_sources_.swap(plan->retired);
    };
    sync_lease_ = lease;
    await_finish(call, terminate, std::move(lease), std::move(plan));
}

void Mbx2dHle::await_finish(UserlandHleCall& call, bool terminate,
    std::shared_ptr<GraphicsCallLease> lease, std::shared_ptr<FinishPlan> plan)
{
    constexpr std::size_t payload_limit = 64U * 1024U * 1024U;
    std::shared_ptr<GlesWorkResult> result;
    try {
        if (!plan->prepared) {
            const auto capacity = retired_client_host_sources_.capacity();
            if (capacity > payload_limit / sizeof(HostSurfaceKey)) {
                lease->consume();
                call.set_return(mbx2d_abi::failure);
                return;
            }
            plan->bytes = capacity * sizeof(HostSurfaceKey);
            plan->credit = reserve_gles_host_preparation(plan->bytes);
            if (!plan->credit) {
                call.suspend_until(
                    [bytes = plan->bytes] {
                        return gles_host_preparation_capacity(bytes);
                    },
                    [this, terminate, lease, plan](UserlandHleCall& resumed) {
                        await_finish(resumed, terminate, lease, plan);
                    });
                return;
            }
            plan->retired.swap(retired_client_host_sources_);
            plan->prepared = true;
        }
        result = enqueue_gles_host_work(
            0,
            [backend = FinishBackend { host_graphics_, command_encoder_ },
                plan](DisplayFrame&) {
                const auto complete = backend.execute(plan->retired);
                plan->retired_completed = complete;
                return complete;
            },
            plan->bytes);
    } catch (...) {
        lease->consume();
        call.set_return(mbx2d_abi::failure);
        return;
    }
    if (!result) {
        call.suspend_until(
            [bytes = plan->bytes] { return gles_host_work_capacity(0, bytes); },
            [this, terminate, lease, plan](UserlandHleCall& resumed) {
                await_finish(resumed, terminate, lease, plan);
            });
        return;
    }
    plan->accepted = true;
    lease->result = result;
    call.suspend_until([result] { return result->ready(); },
        [this, result, lease, plan, terminate](UserlandHleCall& resumed) {
            const auto same_owner = plan->owner == renderer_owner_;
            const auto success = result->succeeded() && same_owner &&
                                 (terminate || finish_destination_live(*plan));
            lease->consume();
            if (same_owner && terminate)
                reset();
            else if (success)
                submit_destination(resumed, false);
            resumed.set_return(
                success ? mbx2d_abi::success : mbx2d_abi::failure);
        },
        result->sequence());
}

} // namespace ilemu
