// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// Choose bounded guest dispatch hints while preserving scheduler
// eligibility.

#include "mach/guest_dispatch_policy.hpp"

#include <stdexcept>

namespace ilemu {
namespace {

    [[nodiscard]] bool runnable(
        const XnuScheduler& scheduler, XnuThreadId thread)
    {
        const auto info = scheduler.info(thread);
        return info && info->state == XnuThreadState::Runnable;
    }

} // namespace

GuestDispatchPolicy::GuestDispatchPolicy(
    std::chrono::nanoseconds response_period)
    : realtime_burst_period_ { response_period }
    , interaction_lease_ { response_period * 8 }
{
    if (response_period <= std::chrono::nanoseconds::zero()) {
        throw std::invalid_argument { "response period must be positive" };
    }
}

GuestDispatchDecision GuestDispatchPolicy::decide_interactive_process(
    const XnuScheduler& scheduler, std::uint32_t process,
    GuestDispatchReason reason)
{
    const auto candidate =
        scheduler.highest_priority_runnable_thread(process);
    if (!candidate) {
        preferred_process_.reset();
        preferred_process_burst_ = 0U;
        return { };
    }
    if (preferred_process_ != process) {
        preferred_process_ = process;
        preferred_process_burst_ = 0U;
    }
    if (preferred_process_burst_ >= preferred_process_burst_limit) {
        // Return one selection to the ordinary XNU queue after every bounded
        // role burst. The queue can still choose this process naturally, but
        // an unrelated higher-priority dependency cannot be starved by host
        // foreground QoS.
        preferred_process_burst_ = 0U;
        return { };
    }
    ++preferred_process_burst_;
    return { candidate, reason, false };
}

GuestDispatchDecision GuestDispatchPolicy::decide(
    const XnuScheduler& scheduler,
    const GuestDispatchObservation& observation)
{
    if (observation.interaction_generation != 0U &&
        observation.interaction_generation !=
            observed_interaction_generation_) {
        observed_interaction_generation_ =
            observation.interaction_generation;
        interaction_deadline_ = observation.now + interaction_lease_;
        // Do not carry the receiver of an older interaction into a new one.
        // Reacquire the process from the real service receive owner; an exact
        // consuming Guest thread can refine the same identity below. No
        // application or firmware identity is inferred here.
        input_process_ = observation.input_receiver_process;
    }

    GuestDispatchDecision result;
    const auto direct = [&](std::optional<XnuThreadId> thread,
                            GuestDispatchReason reason) {
        preferred_process_.reset();
        preferred_process_burst_ = 0U;
        result.preferred_thread = thread;
        result.reason = reason;
        return result;
    };
    const auto realtime = [&](std::optional<XnuThreadId> thread,
                              GuestDispatchReason reason) {
        if (thread && observation.active_process == thread->process &&
            (!observation.foreground_transition_process ||
                observation.foreground_transition_process == thread->process)) {
            // Preserve callback locality for the settled foreground owner.
            realtime_burst_deadline_.reset();
            return direct(thread, reason);
        }
        if (!realtime_burst_deadline_)
            realtime_burst_deadline_ =
                observation.now + realtime_burst_period_;
        if (observation.now >= *realtime_burst_deadline_) {
            // A stream of notifications can continuously renew its callback
            // lease, including while another process must finish a foreground
            // handoff. Bound that realtime dependency chain independently
            // of those leases. An ordinary priority selection can pick the
            // callback again while a lower-priority service worker is waiting;
            // give the oldest other runnable continuation one bounded turn.
            // Changing callback, receiver or process must not restart the burst.
            realtime_burst_deadline_.reset();
            const auto dependency = thread
                ? scheduler.oldest_runnable_thread_except(*thread)
                : std::nullopt;
            return direct(dependency, dependency
                ? GuestDispatchReason::RealtimeDependency
                : GuestDispatchReason::Ordinary);
        }
        return direct(thread, reason);
    };
    if (observation.debugger_thread) {
        return direct(observation.debugger_thread,
            GuestDispatchReason::Debugger);
    }

    if (observation.explicit_handoff_thread) {
        if (runnable(scheduler, *observation.explicit_handoff_thread)) {
            return direct(observation.explicit_handoff_thread,
                GuestDispatchReason::ExplicitHandoff);
        }
        result.explicit_handoff_stale = true;
    }

    if (observation.input_target_thread &&
        runnable(scheduler, *observation.input_target_thread)) {
        input_process_ = observation.input_target_thread->process;
        interaction_deadline_ = observation.now + interaction_lease_;
        if (++input_target_burst_ > preferred_process_burst_limit) {
            if (input_target_burst_ == preferred_process_burst_limit + 1U) {
                // Firmware callbacks can spin on work owned by another
                // thread. Give that dependency a turn even at lower priority.
                if (const auto dependency = scheduler.oldest_runnable_thread(
                        *input_process_, observation.input_target_thread))
                    return direct(dependency, GuestDispatchReason::InputProcess);
            }
            // Also return one selection to XNU for cross-process dependencies
            // and rendering; a pending callback must not monopolize dispatch.
            input_target_burst_ = 0U;
            return direct(std::nullopt, GuestDispatchReason::Ordinary);
        }
        return direct(observation.input_target_thread,
            GuestDispatchReason::InputTarget);
    }
    input_target_burst_ = 0U;

    if (observation.frame_dependency_thread &&
        runnable(scheduler, *observation.frame_dependency_thread)) {
        // A compositor wake must not hide a queued foreground frame callback.
        // Follow the real receiver, including after touch input has ended, but
        // return every fourth decision to the existing realtime/XNU paths.
        if (++frame_dependency_burst_ <= preferred_process_burst_limit)
            return direct(observation.frame_dependency_thread,
                GuestDispatchReason::FrameDependency);
    }
    frame_dependency_burst_ = 0U;

    if (!observation.active_process) {
        if (!realtime_burst_deadline_)
            realtime_burst_deadline_ =
                observation.now + realtime_burst_period_;
        if (observation.now >= *realtime_burst_deadline_) {
            // Native resume can wait for service workers after the display
            // callback lease expires. Keep handoff fairness until firmware
            // establishes foreground ownership, even without a realtime hint.
            realtime_burst_deadline_.reset();
            const auto dependency = scheduler.oldest_runnable_thread_except();
            return direct(dependency, dependency
                ? GuestDispatchReason::ForegroundTransition
                : GuestDispatchReason::Ordinary);
        }
    }

    if (observation.realtime_work_pending &&
        observation.realtime_yielded_thread && observation.realtime_process) {
        if (const auto dependency =
                scheduler.highest_priority_runnable_thread(
                    *observation.realtime_process,
                    observation.realtime_yielded_thread)) {
            return realtime(dependency,
                GuestDispatchReason::RealtimeYieldDependency);
        }
    }

    if (observation.realtime_inflight && observation.realtime_inflight_thread &&
        observation.realtime_inflight_thread !=
            observation.realtime_yielded_thread &&
        runnable(scheduler, *observation.realtime_inflight_thread)) {
        return realtime(observation.realtime_inflight_thread,
            GuestDispatchReason::RealtimeInflight);
    }

    if (observation.realtime_notification_pending &&
        observation.realtime_receiver_thread &&
        observation.realtime_receiver_thread !=
            observation.realtime_yielded_thread &&
        runnable(scheduler, *observation.realtime_receiver_thread)) {
        return realtime(observation.realtime_receiver_thread,
            GuestDispatchReason::RealtimeReceiver);
    }

    if (observation.realtime_callback_thread &&
        observation.realtime_callback_thread !=
            observation.realtime_yielded_thread &&
        runnable(scheduler, *observation.realtime_callback_thread)) {
        return realtime(observation.realtime_callback_thread,
            GuestDispatchReason::RealtimeCallback);
    }

    if (observation.realtime_callback_thread && observation.realtime_process &&
        (observation.realtime_notification_pending ||
            observation.realtime_lease_active)) {
        const auto callback_info =
            scheduler.info(*observation.realtime_callback_thread);
        if (callback_info && callback_info->state != XnuThreadState::Runnable) {
            if (observation.realtime_receiver_thread &&
                runnable(scheduler, *observation.realtime_receiver_thread)) {
                return realtime(observation.realtime_receiver_thread,
                    GuestDispatchReason::RealtimeReceiver);
            }
            if (const auto dependency =
                    scheduler.highest_priority_runnable_thread(
                        *observation.realtime_process,
                        observation.realtime_callback_thread)) {
                return realtime(dependency,
                    GuestDispatchReason::RealtimeDependency);
            }
        }
    }

    if (observation.realtime_inflight && observation.realtime_inflight_thread &&
        observation.realtime_process) {
        const auto callback_info =
            scheduler.info(*observation.realtime_inflight_thread);
        if (callback_info && callback_info->state != XnuThreadState::Runnable) {
            if (const auto dependency =
                    scheduler.highest_priority_runnable_thread(
                        *observation.realtime_process,
                        observation.realtime_inflight_thread)) {
                return realtime(dependency,
                    GuestDispatchReason::RealtimeDependency);
            }
        }
    }

    if (observation.active_process)
        realtime_burst_deadline_.reset();

    if (observation.foreground_transition_process) {
        auto decision = decide_interactive_process(scheduler,
            *observation.foreground_transition_process,
            GuestDispatchReason::ForegroundTransition);
        decision.explicit_handoff_stale = result.explicit_handoff_stale;
        return decision;
    }
    if (input_process_ && observation.now < interaction_deadline_) {
        // Input delivery often wakes a run-loop receiver which then hands work
        // to another thread in the same process. Keep that real dependency
        // preferred for a bounded lease instead of discarding all input
        // locality after one exact-thread dispatch. If it blocks, immediately
        // fall through to the active process; if it remains runnable, the
        // existing burst limit still yields every fourth selection to XNU's
        // ordinary queue.
        if (scheduler.highest_priority_runnable_thread(*input_process_)) {
            auto decision = decide_interactive_process(scheduler,
                *input_process_, GuestDispatchReason::InputProcess);
            decision.explicit_handoff_stale = result.explicit_handoff_stale;
            return decision;
        }
        input_process_.reset();
    }
    if (observation.active_process &&
        observation.now < interaction_deadline_) {
        auto decision = decide_interactive_process(scheduler,
            *observation.active_process,
            GuestDispatchReason::InteractiveProcess);
        decision.explicit_handoff_stale = result.explicit_handoff_stale;
        return decision;
    }

    preferred_process_.reset();
    preferred_process_burst_ = 0U;
    return result;
}

} // namespace ilemu
