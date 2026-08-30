#include "ilemu/guest_dispatch_policy.hpp"

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
    : interaction_lease_ { response_period * 8 }
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

    if (observation.realtime_work_pending &&
        observation.realtime_yielded_thread && observation.realtime_process) {
        if (const auto dependency =
                scheduler.oldest_runnable_thread(*observation.realtime_process,
                    observation.realtime_yielded_thread)) {
            return direct(dependency,
                GuestDispatchReason::RealtimeYieldDependency);
        }
    }

    if (observation.realtime_inflight && observation.realtime_inflight_thread &&
        observation.realtime_inflight_thread !=
            observation.realtime_yielded_thread &&
        runnable(scheduler, *observation.realtime_inflight_thread)) {
        return direct(observation.realtime_inflight_thread,
            GuestDispatchReason::RealtimeInflight);
    }

    if (observation.realtime_notification_pending &&
        observation.realtime_receiver_thread &&
        observation.realtime_receiver_thread !=
            observation.realtime_yielded_thread &&
        runnable(scheduler, *observation.realtime_receiver_thread)) {
        return direct(observation.realtime_receiver_thread,
            GuestDispatchReason::RealtimeReceiver);
    }

    if (observation.realtime_callback_thread &&
        observation.realtime_callback_thread !=
            observation.realtime_yielded_thread &&
        runnable(scheduler, *observation.realtime_callback_thread)) {
        return direct(observation.realtime_callback_thread,
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
                return direct(observation.realtime_receiver_thread,
                    GuestDispatchReason::RealtimeReceiver);
            }
            if (const auto dependency = scheduler.oldest_runnable_thread(
                    *observation.realtime_process,
                    observation.realtime_callback_thread)) {
                return direct(dependency,
                    GuestDispatchReason::RealtimeDependency);
            }
        }
    }

    if (observation.realtime_inflight && observation.realtime_inflight_thread &&
        observation.realtime_process) {
        const auto callback_info =
            scheduler.info(*observation.realtime_inflight_thread);
        if (callback_info && callback_info->state != XnuThreadState::Runnable) {
            if (const auto dependency = scheduler.oldest_runnable_thread(
                    *observation.realtime_process,
                    observation.realtime_inflight_thread)) {
                return direct(dependency,
                    GuestDispatchReason::RealtimeDependency);
            }
        }
    }

    if (observation.input_target_thread &&
        runnable(scheduler, *observation.input_target_thread)) {
        return direct(observation.input_target_thread,
            GuestDispatchReason::InputTarget);
    }

    if (observation.foreground_transition_process) {
        auto decision = decide_interactive_process(scheduler,
            *observation.foreground_transition_process,
            GuestDispatchReason::ForegroundTransition);
        decision.explicit_handoff_stale = result.explicit_handoff_stale;
        return decision;
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
