#include "ilemu/bootstrap_mig_ids.hpp"
#include "ilemu/darwin_abi.hpp"
#include "ilemu/darwin_kqueue_abi.hpp"
#include "ilemu/darwin_network_abi.hpp"
#include "ilemu/darwin_resource_abi.hpp"
#include "ilemu/darwin_route_socket.hpp"
#include "ilemu/kernel.hpp"
#include "ilemu/kernel_clock.hpp"
#include "ilemu/kernel_iokit.hpp"
#include "ilemu/kernel_mach_ipc.hpp"
#include "ilemu/kernel_network.hpp"
#include "ilemu/mach_clock_abi.hpp"
#include "ilemu/mach_host_mig_ids.hpp"
#include "ilemu/mach_port_mig_ids.hpp"
#include "ilemu/mach_scheduler_abi.hpp"
#include "ilemu/mach_thread_policy_abi.hpp"
#include "ilemu/mig_wire_abi.hpp"
#include "ilemu/task_mig_ids.hpp"
#include "ilemu/thread_act_mig_ids.hpp"
#include "ilemu/vm_map_mig_ids.hpp"
#include "ilemu/xnu_mig_adapter.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <limits>
#include <span>
#include <sstream>
#include <string_view>
#include <utility>
#include <vector>

#include "support.hpp"

namespace ilemu {
namespace {

    constexpr std::uint32_t maximum_semaphore_wait_traces = 128;

} // namespace

using namespace mach_support;

std::optional<CompatibilityKernel::SchedulerYieldRequest>
CompatibilityKernel::consume_scheduler_yield(std::size_t processor_id)
{
    const auto request = scheduler_yields_.find(processor_id);
    if (request == scheduler_yields_.end())
        return std::nullopt;
    auto result = request->second;
    scheduler_yields_.erase(request);
    return result;
}

std::optional<XnuThreadId> CompatibilityKernel::consume_scheduler_handoff(
    std::size_t processor_id)
{
    const auto handoff = scheduler_handoffs_.find(processor_id);
    if (handoff == scheduler_handoffs_.end())
        return std::nullopt;
    const auto result = handoff->second;
    scheduler_handoffs_.erase(handoff);
    return result;
}

std::uint32_t CompatibilityKernel::signal_semaphore_locked(std::uint32_t name,
    bool all, bool prepost, std::optional<WokenThread>* woken_thread,
    std::vector<WokenThread>* woken_threads)
{
    const auto object = resolve_name_with_right(
        *shared_state_, process_.pid, name, xnu792::ipc::Right::Send);
    if (!object)
        return 4; // KERN_INVALID_ARGUMENT
    const auto semaphore = shared_state_->mach_semaphores.find(*object);
    if (semaphore == shared_state_->mach_semaphores.end()) {
        return 4; // KERN_INVALID_ARGUMENT
    }
    auto& waiters = semaphore->second.waiters;
    if (all) {
        if (woken_threads)
            woken_threads->reserve(waiters.size());
        for (const auto& waiter : waiters) {
            shared_state_->semaphore_wakeups.insert(waiter);
            if (woken_threads)
                woken_threads->push_back(waiter);
        }
        waiters.clear();
        // semaphore_signal_all never leaves a prepost behind.
        semaphore->second.count = 0;
    } else if (!waiters.empty()) {
        shared_state_->semaphore_wakeups.insert(waiters.front());
        if (woken_thread)
            *woken_thread = waiters.front();
        waiters.pop_front();
    } else if (prepost) {
        ++semaphore->second.count;
    }
    return 0;
}

std::uint32_t CompatibilityKernel::signal_semaphore_thread_locked(
    std::uint32_t semaphore_name, std::uint32_t thread_name,
    std::optional<WokenThread>* woken_thread)
{
    constexpr std::uint32_t kern_invalid_argument = 4;
    constexpr std::uint32_t kern_not_waiting = 48;
    const auto semaphore_object = resolve_name_with_right(
        *shared_state_, process_.pid, semaphore_name, xnu792::ipc::Right::Send);
    if (!semaphore_object)
        return kern_invalid_argument;
    const auto semaphore =
        shared_state_->mach_semaphores.find(*semaphore_object);
    if (semaphore == shared_state_->mach_semaphores.end())
        return kern_invalid_argument;

    std::optional<std::pair<std::uint32_t, std::uint32_t>> target;
    if (thread_name != xnu792::ipc::null_name) {
        const auto thread_object = resolve_name_with_right(*shared_state_,
            process_.pid, thread_name, xnu792::ipc::Right::Send);
        if (!thread_object)
            return kern_invalid_argument;
        target = find_thread_owner(*shared_state_, *thread_object);
        if (!target)
            return kern_invalid_argument;
    }

    auto& waiters = semaphore->second.waiters;
    const auto waiter = target
                            ? std::find(waiters.begin(), waiters.end(), *target)
                            : waiters.begin();
    if (waiter == waiters.end())
        return kern_not_waiting;
    shared_state_->semaphore_wakeups.insert(*waiter);
    if (woken_thread)
        *woken_thread = *waiter;
    waiters.erase(waiter);
    return 0;
}

void CompatibilityKernel::wake_thread_and_maybe_preempt(
    Cpu& cpu, const std::optional<WokenThread>& thread)
{
    if (!thread || !thread_wake_handler_)
        return;
    const auto wake_result =
        thread_wake_handler_(thread->first, thread->second);
    if (wake_result.preemption_needed && scheduler_preemption_query_ &&
        scheduler_preemption_query_(cpu.processor_id())) {
        // The wake happened in the active CPU's HLE/SVC dispatch. Request the
        // same AST boundary used by thread creation and policy changes; the
        // main loop will complete the target's pending wait before dispatching
        // it.
        cpu.request_guest_preemption();
    }
}

void CompatibilityKernel::wake_threads_and_maybe_preempt(
    Cpu& cpu, std::span<const WokenThread> threads)
{
    if (!thread_wake_handler_)
        return;
    bool preemption_needed = false;
    for (const auto& [process, processor] : threads) {
        preemption_needed |=
            thread_wake_handler_(process, processor).preemption_needed;
    }
    if (preemption_needed && scheduler_preemption_query_ &&
        scheduler_preemption_query_(cpu.processor_id())) {
        // The wake happened in the active CPU's HLE/SVC dispatch. Request the
        // same AST boundary used by thread creation and policy changes; the
        // main loop will complete the target's pending wait before dispatching
        // it.
        cpu.request_guest_preemption();
    }
}

void CompatibilityKernel::wait_on_semaphore(Cpu& cpu, std::uint32_t wait_name,
    std::uint32_t signal_name, std::optional<std::uint64_t> timeout_interval,
    bool bsd_result)
{
    constexpr std::uint32_t kern_invalid_argument = 4;
    constexpr std::uint32_t kern_operation_timed_out = 49;
    std::uint32_t result = 0;
    bool blocked = false;
    std::optional<WokenThread> woken_thread;
    {
        std::lock_guard mach_lock { shared_state_->mach_mutex };
        const auto wait_object = resolve_name_with_right(
            *shared_state_, process_.pid, wait_name, xnu792::ipc::Right::Send);
        const auto signal_object =
            signal_name == 0
                ? std::optional<std::uint32_t> { }
                : resolve_name_with_right(*shared_state_, process_.pid,
                      signal_name, xnu792::ipc::Right::Send);
        const auto wait =
            wait_object ? shared_state_->mach_semaphores.find(*wait_object)
                        : shared_state_->mach_semaphores.end();
        if (wait == shared_state_->mach_semaphores.end() ||
            (signal_name != 0 && !signal_object)) {
            result = kern_invalid_argument;
        } else if (wait->second.count > 0) {
            --wait->second.count;
        } else if (timeout_interval && *timeout_interval == 0) {
            result = kern_operation_timed_out;
        } else {
            const auto processor =
                static_cast<std::uint32_t>(cpu.processor_id());
            wait->second.waiters.emplace_back(process_.pid, processor);
            pending_semaphore_waits_[cpu.processor_id()] =
                PendingSemaphoreWait { *wait_object, cpu.processor_id(),
                    timeout_interval
                        ? std::optional<std::uint64_t> { shared_state_->clock
                                                             .now() +
                                                         *timeout_interval }
                        : std::nullopt,
                    bsd_result };
            blocked = true;
        }
        // XNU first establishes/consumes the wait and only then performs the
        // paired signal, keeping pthread condition-variable handoff atomic.
        if (result == 0 && signal_name != 0) {
            result = signal_semaphore_locked(
                signal_name, false, true, &woken_thread);
        }
    }

    wake_thread_and_maybe_preempt(cpu, woken_thread);

    if (result != 0) {
        if (bsd_result) {
            bsd_error(cpu, result == kern_operation_timed_out
                               ? 60U
                               : darwin::error::invalid_argument);
        } else {
            cpu.registers()[0] = result;
        }
        return;
    }
    if (!blocked) {
        if (bsd_result)
            bsd_success(cpu, 0);
        else
            cpu.registers()[0] = 0;
        return;
    }
    process_.waiting_for_events = true;
    if (semaphore_wait_trace_count_ < maximum_semaphore_wait_traces) {
        output_.write(
            "[semaphore] wait pid=" + std::to_string(process_.pid) +
            " cpu=" + std::to_string(cpu.processor_id()) +
            " sem=" + std::to_string(wait_name) +
            (signal_name == 0 ? std::string { }
                              : " signal=" + std::to_string(signal_name)) +
            "\n");
        ++semaphore_wait_trace_count_;
    }
    cpu.halt(Dynarmic::HaltReason::UserDefined5);
}

} // namespace ilemu
