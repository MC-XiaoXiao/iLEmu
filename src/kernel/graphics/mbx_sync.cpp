// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "kernel/mbx2d_hle.hpp"

#include "foundation/cpu.hpp"
#include "graphics/display.hpp"
#include "graphics/gles_renderer.hpp"
#include "graphics/presentation_tracker.hpp"

#include <stdexcept>
#include <utility>

namespace ilemu {
namespace {
    struct SubmissionBackend {
        // The encoder can reference its renderer; destruction order is
        // explicit.
        std::shared_ptr<GlesRenderer> renderer;
        std::shared_ptr<GlesRendererOwner> lifetime;
        std::shared_ptr<CommandEncoder> encoder;

        bool execute(bool finish_gpu,
            std::span<const HostSurfaceKey> retired) const
        {
            if (!finish_gpu)
                return encoder && encoder->submit(PerfSubmitReason::MbxFlush);
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

bool Mbx2dHle::submission_destination_live(
    UserlandHleCall& call, const SubmissionPlan& plan)
{
    const auto* state = select_state(call, plan.context_api);
    if (!state)
        return false;
    const auto current = resolve(state->destination);
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

void Mbx2dHle::submit(
    UserlandHleCall& call, SubmissionKind kind, bool context_api)
{
    auto lease = std::make_shared<GraphicsCallLease>();
    lease->owner = call.cpu().processor_id();
    auto plan = std::make_shared<SubmissionPlan>();
    plan->owner = renderer_owner_;
    plan->kind = kind;
    plan->context_api = context_api;
    const auto* state = select_state(call, context_api);
    if (!state) {
        call.set_return(mbx2d_abi::failure);
        return;
    }
    const auto destination = resolve(state->destination);
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
        if ((plan->kind == SubmissionKind::Finish ||
                plan->kind == SubmissionKind::Terminate) &&
            plan->prepared && (!plan->accepted || !plan->retired_completed) &&
            plan->owner == renderer_owner_)
            retired_client_host_sources_.swap(plan->retired);
    };
    sync_lease_ = lease;
    await_submission(call, std::move(lease), std::move(plan));
}

void Mbx2dHle::await_submission(UserlandHleCall& call,
    std::shared_ptr<GraphicsCallLease> lease, std::shared_ptr<SubmissionPlan> plan)
{
    constexpr std::size_t payload_limit = 64U * 1024U * 1024U;
    std::shared_ptr<GlesWorkResult> result;
    try {
        if (plan->owner != renderer_owner_ || lease->cancelled || lease->consumed)
            throw std::runtime_error { "stale MBX submission" };
        const auto finish_gpu = plan->kind == SubmissionKind::Finish ||
                                plan->kind == SubmissionKind::Terminate;
        if (!plan->prepared) {
            const auto capacity = finish_gpu
                                      ? retired_client_host_sources_.capacity()
                                      : 0;
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
                    [this, lease, plan](UserlandHleCall& resumed) {
                        await_submission(resumed, lease, plan);
                    });
                return;
            }
            if (finish_gpu)
                plan->retired.swap(retired_client_host_sources_);
            plan->prepared = true;
        }
        result = enqueue_gles_host_work(
            0,
            [backend = SubmissionBackend {
                 host_graphics_, renderer_lifetime_, command_encoder_ },
                plan, finish_gpu](DisplayFrame&) {
                const auto complete = backend.execute(finish_gpu, plan->retired);
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
            [this, lease, plan](UserlandHleCall& resumed) {
                await_submission(resumed, lease, plan);
            });
        return;
    }
    plan->accepted = true;
    lease->result = result;
    call.suspend_until([result] { return result->ready(); },
        [this, result, lease, plan](UserlandHleCall& resumed) {
            const auto terminate = plan->kind == SubmissionKind::Terminate;
            const auto same_owner = plan->owner == renderer_owner_;
            const auto success = result->succeeded() && same_owner &&
                                 (terminate ||
                                     submission_destination_live(resumed, *plan));
            lease->consume();
            if (same_owner && terminate)
                reset();
            else if (success) {
                submit_destination(resumed, plan->context_api);
                if (plan->kind == SubmissionKind::Swap && display_ &&
                    (!presentation_tracker_ ||
                        !presentation_tracker_->has_presented_frame()))
                    display_->present(resumed.process_id());
            }
            resumed.set_return(
                success ? mbx2d_abi::success : mbx2d_abi::failure);
        },
        result->sequence());
}

} // namespace ilemu
