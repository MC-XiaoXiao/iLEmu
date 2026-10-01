// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// Manage guest signal masks, actions, delivery and thread
// interruption.
//
// Apple public ABI/behavior references (guest profiles may differ):
// https://github.com/apple-oss-distributions/xnu/blob/xnu-792.24.17/bsd/kern/kern_sig.c

#include "kernel/kernel.hpp"

#include "kernel/darwin_abi.hpp"
#include "kernel/darwin_signal_context.hpp"
#include "../../mach/support.hpp"
#include <algorithm>
#include <bit>
#include <cstdint>
#include <limits>
#include <vector>

#include "../support.hpp"

namespace ilemu {
namespace {

    bool default_signal_is_ignored(std::uint32_t signal)
    {
        using namespace darwin::signal;
        return signal == urgent || signal == child || signal == io ||
               signal == window_change || signal == information ||
               signal == resume;
    }

    bool default_signal_stops(std::uint32_t signal)
    {
        using namespace darwin::signal;
        return signal == stop || signal == terminal_stop ||
               signal == terminal_input || signal == terminal_output;
    }

} // namespace

void CompatibilityKernel::dispatch_bsd_signal_mask(Cpu& cpu, std::uint32_t number)
{
    const auto& registers = cpu.registers();
    const auto processor = cpu.processor_id();
    const auto previous = signal_state_.mask(processor);
    const bool broadcast = registers[1] != 0 &&
                           number != darwin::syscall::pthread_sigmask;
    if (registers[1] != 0) {
        // XNU reads the new mask before touching an aliased old-mask output.
        const auto requested = memory_.read32(registers[1]);
        if (!requested) {
            bsd_error(cpu, darwin::error::bad_address);
            return;
        }
        const auto scope = number == darwin::syscall::pthread_sigmask
                               ? DarwinSignalState::Scope::Thread
                               : DarwinSignalState::Scope::Process;
        if (!signal_state_.update(processor, registers[0], *requested, scope)) {
            bsd_error(cpu, darwin::error::invalid_argument);
            return;
        }
    }
    // kern_sig.c deliberately ignores copyout failure for both mask calls.
    if (registers[2] != 0)
        static_cast<void>(memory_.write32(registers[2], previous));
    bsd_success(cpu, 0);
    if (broadcast)
        signal_state_.for_each_ready([&](std::size_t target) {
            if (process_pending_signals(target))
                cpu.request_guest_preemption();
        });
}

void CompatibilityKernel::discard_ignored_signal(std::uint32_t signal)
{
    const auto handler = signal_actions_[signal][0];
    if (handler == darwin::signal::ignore_action ||
        (handler == darwin::signal::default_action && default_signal_is_ignored(signal)))
        signal_state_.discard(1U << (signal - 1U));
}

void CompatibilityKernel::reset_signal_actions_for_exec()
{
    for (std::size_t signal = 1; signal < signal_actions_.size(); ++signal) {
        auto& action = signal_actions_[signal];
        if (action[0] != darwin::signal::default_action &&
            action[0] != darwin::signal::ignore_action) {
            action = { };
            discard_ignored_signal(static_cast<std::uint32_t>(signal));
        }
    }
}

void CompatibilityKernel::apply_spawn_signal_attributes(
    const SpawnSignalAttributes& attributes)
{
    if (attributes.mask)
        static_cast<void>(signal_state_.update(0, 3, *attributes.mask,
            DarwinSignalState::Scope::Process));
    for (std::size_t signal = 1; signal < signal_actions_.size(); ++signal) {
        if ((attributes.defaults & (1U << (signal - 1U))) != 0U)
            signal_actions_[signal] = { };
    }
}

std::uint32_t CompatibilityKernel::deliver_signal(std::uint32_t signal,
    std::uint32_t sender_pid, std::uint32_t sender_uid)
{
    return deliver_signal_to_thread(signal, std::nullopt, sender_pid, sender_uid);
}

bool CompatibilityKernel::transition_signal_stop(bool stopped, std::uint32_t signal)
{
    bool changed = false;
    bool notified = false;
    {
        std::lock_guard mach_lock { shared_state_->mach_mutex };
        const auto record = shared_state_->processes.find(process_.pid);
        if (record != shared_state_->processes.end() && !record->second.exited) {
            auto& state = record->second;
            changed = state.signal_stopped != stopped;
            state.signal_stopped = stopped;
            if (stopped && changed) {
                state.child_wait_status.stopped(signal);
                const auto parent = shared_state_->processes.find(state.parent_pid);
                if (parent != shared_state_->processes.end() &&
                    !parent->second.signal_stopped) {
                    ++parent->second.child_wait_generation;
                    notified = true;
                }
            } else if (!stopped && (changed || signal == darwin::signal::resume)) {
                state.child_wait_status.continued();
            }
        }
    }
    if (notified)
        shared_state_->note_io_event_transition();
    if (changed && process_runnable_handler_)
        process_runnable_handler_(process_.pid, !stopped);
    if (changed && stopped && child_status_handler_)
        child_status_handler_(process_.parent_pid,
            { ChildStatus::Kind::Stop, process_.pid, process_.uid, signal });
    return changed;
}

std::uint32_t CompatibilityKernel::deliver_signal_to_thread(
    std::uint32_t signal, std::optional<std::size_t> processor,
    std::uint32_t sender_pid, std::uint32_t sender_uid)
{
    if (signal == 0 || signal >= darwin::signal::count)
        return signal == 0 ? 0U : darwin::error::invalid_argument;
    if (process_.exited)
        return 0;

    const auto handler = signal_actions_[signal][0];
    if (signal != darwin::signal::resume &&
        (handler == darwin::signal::ignore_action ||
            (handler == darwin::signal::default_action &&
                default_signal_is_ignored(signal))))
        return 0;

    const auto bit = 1U << (signal - 1U);
    if (!processor)
        processor = signal_state_.select(bit, [this](std::size_t candidate) {
            return !disabled_thread_signals_.contains(candidate);
        });
    if (!processor) {
        if (signal == darwin::signal::resume)
            transition_signal_stop(false);
        return 0;
    }
    if (signal == darwin::signal::resume) {
        // kern_sig.c: a held SIGCONT still resumes a stopped task, but a
        // held signal on an already running task does not set P_CONTINUED.
        // sigwait selection precedes the mask check in native psignal.
        const bool selected = (signal_state_.waiting_for(*processor) & bit) != 0;
        const bool held = (signal_state_.mask(*processor) & bit) != 0;
        transition_signal_stop(false, (!held || selected) ? signal : 0U);
    }
    // XNU psignal selects one uthread, coalesces duplicates, and cancels
    // opposing stop/continue bits on that uthread, even when held.
    signal_state_.queue(*processor, bit);
    // kern_sig.c keeps ordinary sender information on the process, not in
    // a fabricated per-signal FIFO. Held signals do not overwrite it.
    if (handler > darwin::signal::ignore_action &&
        !(signal_state_.mask(*processor) & bit) && signal != darwin::signal::child)
        signal_sender_ = { sender_pid, sender_uid, signal };
    static_cast<void>(process_pending_signals(*processor));
    refresh_pending_event_processor_locked(*processor);
    return 0;
}

bool CompatibilityKernel::process_pending_signals(std::size_t processor)
{
    auto ready = signal_state_.ready(processor);
    while (ready && !process_.exited) {
        const auto signal = static_cast<std::uint32_t>(std::countr_zero(ready)) + 1U;
        const auto bit = 1U << (signal - 1U);
        ready &= ~bit;
        const auto waiting = pending_signal_waits_.find(processor);
        if (waiting != pending_signal_waits_.end() &&
            (signal_state_.waiting_for(processor) & bit)) {
            waiting->second.selected = signal;
            signal_state_.accept_wait_signal(processor, bit);
            shared_state_->note_io_event_transition();
            continue;
        }
        const auto handler = signal_actions_[signal][0];
        if (handler != darwin::signal::default_action &&
            handler != darwin::signal::ignore_action) {
            // Publish an AST-style event; only a safe user-return boundary
            // may consume this bit and install the ARM frame.
            shared_state_->note_io_event_transition();
            refresh_pending_event_processor_locked(processor);
            if (const auto suspended = pending_signal_suspends_.find(processor);
                suspended != pending_signal_suspends_.end() &&
                !suspended->second.interrupted) {
                suspended->second.interrupted = true;
                shared_state_->note_io_event_transition();
            }
            if (waiting != pending_signal_waits_.end() &&
                !waiting->second.interruption_result) {
                waiting->second.interruption_result =
                    (signal_actions_[signal][3] & darwin::signal::restart_action_flag)
                        ? 0U : darwin::error::interrupted;
                shared_state_->note_io_event_transition();
            }
            continue;
        }
        signal_state_.consume(processor, bit);
        if (handler == darwin::signal::ignore_action || default_signal_is_ignored(signal))
            continue;
        if (default_signal_stops(signal)) {
            transition_signal_stop(true, signal);
            return true;
        }
        exit_process(0, signal);
    }
    return false;
}

void CompatibilityKernel::dispatch_bsd_signal(Cpu& cpu, std::uint32_t number)
{
    if (number == darwin::signal_context::syscall) {
        dispatch_bsd_signal_return(cpu);
        return;
    }
    if (number == 330U || number == 422U) {
        dispatch_bsd_signal_wait(cpu);
        return;
    }
    if (number == 52U) { // sigpending reports this uthread's list, including held signals.
        // XNU kern_sig.c intentionally ignores copyout failure here.
        if (cpu.registers()[0] != 0)
            static_cast<void>(memory_.write32(cpu.registers()[0],
                signal_state_.pending(cpu.processor_id())));
        bsd_success(cpu, 0);
        return;
    }
    if (number == darwin::syscall::alternate_signal_stack) {
        const auto new_address = cpu.registers()[0];
        const auto old_address = cpu.registers()[1];
        const auto processor = cpu.processor_id();
        const auto existing = alternate_signal_stacks_.find(processor);
        const auto old_stack = existing == alternate_signal_stacks_.end()
                                   ? AlternateSignalStack { 0, 0,
                                         darwin::signal::alternate_stack_disabled }
                                   : existing->second;

        // XNU copies the old stack out before reading the new one, including
        // when both user pointers alias. Keep the same observable ordering.
        if (old_address != 0) {
            if (old_address > std::numeric_limits<std::uint32_t>::max() - 8U ||
                !memory_.write32(old_address, old_stack.address) ||
                !memory_.write32(old_address + 4U, old_stack.size) ||
                !memory_.write32(old_address + 8U, old_stack.flags)) {
                bsd_error(cpu, darwin::error::bad_address);
                return;
            }
        }
        if (new_address == 0) {
            bsd_success(cpu, 0);
            return;
        }
        if (new_address > std::numeric_limits<std::uint32_t>::max() - 8U) {
            bsd_error(cpu, darwin::error::bad_address);
            return;
        }
        const auto address = memory_.read32(new_address);
        const auto size = memory_.read32(new_address + 4U);
        const auto flags = memory_.read32(new_address + 8U);
        if (!address || !size || !flags) {
            bsd_error(cpu, darwin::error::bad_address);
            return;
        }
        if ((*flags & ~darwin::signal::alternate_stack_disabled) != 0) {
            bsd_error(cpu, darwin::error::invalid_argument);
            return;
        }
        if ((*flags & darwin::signal::alternate_stack_disabled) != 0) {
            if ((old_stack.flags & darwin::signal::alternate_stack_on_stack) !=
                0) {
                bsd_error(cpu, darwin::error::invalid_argument);
                return;
            }
            alternate_signal_stacks_[processor] =
                AlternateSignalStack { old_stack.address, old_stack.size,
                    darwin::signal::alternate_stack_disabled };
        } else {
            if ((old_stack.flags & darwin::signal::alternate_stack_on_stack) !=
                0) {
                bsd_error(cpu, darwin::error::operation_not_permitted);
                return;
            }
            if (*size < darwin::signal::alternate_stack_minimum_size) {
                bsd_error(cpu, darwin::error::no_memory);
                return;
            }
            alternate_signal_stacks_[processor] =
                AlternateSignalStack { *address, *size, 0 };
        }
        bsd_success(cpu, 0);
        return;
    }
    if (number == 111U) { // sigsuspend
        signal_state_.suspend(cpu.processor_id(), cpu.registers()[0]);
        pending_signal_suspends_[cpu.processor_id()] =
            PendingSignalSuspend { false };
        process_.waiting_for_events = true;
        output_.write("[signal] suspend pid=" + std::to_string(process_.pid) +
                      " cpu=" + std::to_string(cpu.processor_id()) + "\n");
        cpu.halt(Dynarmic::HaltReason::UserDefined5);
        return;
    }
    if (number == darwin::syscall::pthread_kill) {
        const auto thread_name = cpu.registers()[0];
        const auto signal = cpu.registers()[1];
        if (signal >= darwin::signal::count) {
            bsd_error(cpu, darwin::error::invalid_argument);
            return;
        }
        std::optional<std::pair<std::uint32_t, std::uint32_t>> target;
        {
            std::lock_guard mach_lock { shared_state_->mach_mutex };
            const auto object = mach_support::resolve_name_with_right(
                *shared_state_, process_.pid, thread_name,
                xnu::ipc::Right::Send);
            if (object)
                target = mach_support::find_thread_owner(*shared_state_, *object);
        }
        // pthread_t is represented by the target thread's send right. POSIX
        // limits pthread_kill to threads in the caller's process; signal zero
        // performs only this liveness check.
        if (!target || target->first != process_.pid) {
            bsd_error(cpu, darwin::error::no_such_process);
            return;
        }
        if (signal != 0) {
            const auto error = deliver_signal_to_thread(signal, target->second,
                process_.pid, process_.uid);
            if (error != 0) {
                bsd_error(cpu, error);
                return;
            }
        }
        bsd_success(cpu, 0);
        if (process_.exited)
            cpu.halt(Dynarmic::HaltReason::UserDefined1);
        return;
    }
    if (number != darwin::syscall::kill) {
        trace_unknown(cpu, "BSD signal syscall", number);
        bsd_error(cpu, bsd_support::not_implemented);
        return;
    }

    const auto requested_pid = static_cast<std::int32_t>(cpu.registers()[0]);
    const auto signal = cpu.registers()[1];
    if (signal >= darwin::signal::count) {
        bsd_error(cpu, darwin::error::invalid_argument);
        return;
    }

    std::vector<std::uint32_t> targets;
    if (requested_pid > 0) {
        targets.push_back(static_cast<std::uint32_t>(requested_pid));
    } else {
        const auto requested_group =
            requested_pid == 0 ? process_.membership->group()
                               : static_cast<std::uint32_t>(
                                     -static_cast<std::int64_t>(requested_pid));
        for (const auto& [pid, record] : shared_state_->processes) {
            if (record.exited) {
                continue;
            }
            if (requested_pid == -1) {
                if (pid <= 1 || pid == process_.pid) {
                    continue;
                }
            } else if (record.membership->group() != requested_group) {
                continue;
            }
            targets.push_back(pid);
        }
    }

    bool found = false;
    bool permitted = false;
    for (const auto target_pid : targets) {
        const auto target = shared_state_->processes.find(target_pid);
        if (target == shared_state_->processes.end()) {
            continue;
        }
        found = true;
        if (target->second.exited) {
            permitted = true;
            continue;
        }
        if (process_.effective_uid != 0 &&
            process_.effective_uid != target->second.uid) {
            continue;
        }
        permitted = true;
        if (signal == 0) {
            continue;
        }
        const auto error = signal_delivery_handler_
                               ? signal_delivery_handler_(target_pid, signal)
                           : target_pid == process_.pid
                               ? deliver_signal(signal, process_.pid, process_.uid)
                               : darwin::error::no_such_process;
        if (error != 0) {
            bsd_error(cpu, error);
            return;
        }
    }

    if (!found) {
        bsd_error(cpu, darwin::error::no_such_process);
        return;
    }
    if (!permitted) {
        bsd_error(cpu, darwin::error::operation_not_permitted);
        return;
    }
    bsd_success(cpu, 0);
    if (process_.exited) {
        cpu.halt(Dynarmic::HaltReason::UserDefined1);
    } else if (std::find(targets.begin(), targets.end(), process_.pid) !=
               targets.end()) {
        bool signal_stopped = false;
        {
            std::lock_guard mach_lock { shared_state_->mach_mutex };
            const auto record = shared_state_->processes.find(process_.pid);
            signal_stopped = record != shared_state_->processes.end() &&
                             record->second.signal_stopped;
        }
        if (signal_stopped)
            cpu.request_guest_preemption();
    }
}

} // namespace ilemu
