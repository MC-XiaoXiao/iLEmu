// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "kernel/kernel.hpp"
#include "kernel/darwin_signal_context.hpp"
#include <algorithm>
#include <bit>
#include <limits>
#include <span>

namespace ilemu {

bool CompatibilityKernel::caught_signal_ready(std::size_t processor) const
{
    auto ready = signal_state_.ready(processor);
    if (!ready || process_.exited)
        return false;
    // Accepted filesystem work owns its buffers until completion. Defer the
    // user AST without repeatedly polling an uncancellable continuation.
    if (pending_file_syncs_.contains(processor) ||
        pending_file_renames_.contains(processor) ||
        pending_filesystem_dispatches_.contains(processor) ||
        pending_file_mappings_.contains(processor))
        return false;
    while (ready) {
        const auto signal = static_cast<std::uint32_t>(std::countr_zero(ready)) + 1U;
        if (signal_actions_[signal][0] > darwin::signal::ignore_action)
            return true;
        ready &= ready - 1U;
    }
    return false;
}

bool CompatibilityKernel::deliver_pending_signal(Cpu& cpu)
{
    using namespace darwin::signal_context;
    const auto processor = cpu.processor_id();
    if (!caught_signal_ready(processor))
        return false;
    auto ready = signal_state_.ready(processor);
    std::uint32_t signal = 0;
    do {
        signal = static_cast<std::uint32_t>(std::countr_zero(ready)) + 1U;
        ready &= ready - 1U;
    } while (signal_actions_[signal][0] <= darwin::signal::ignore_action);
    const auto bit = 1U << (signal - 1U);
    const auto action = signal_actions_[signal];
    const auto return_mask = signal_state_.inherited_mask(processor);
    const auto suspended = pending_signal_suspends_.contains(processor);
    const auto temporary_mask = signal_state_.mask(processor);
    bool completed = false;
    if (has_pending_event_locked(processor) || pending_waits_.contains(processor)) {
        completed = interrupt_thread_wait(cpu,
            (action[3] & darwin::signal::restart_action_flag) != 0);
        // A granted combined Mach send may have started its receive phase.
        if (completed && pending_mach_receives_.contains(processor))
            completed = interrupt_thread_wait(cpu, false) || completed;
        if (has_pending_event_locked(processor) || pending_waits_.contains(processor))
            return completed;
    }
    const auto handler_mask = suspended ? temporary_mask : signal_state_.mask(processor);
    Frame frame { };
    std::copy(cpu.registers().begin(), cpu.registers().end(), frame.machine.general.begin());
    frame.machine.general[darwin::arm_thread::cpsr_index] = cpu.cpsr();
    frame.machine.floating = cpu.extension_registers();
    frame.machine.fpscr = cpu.fpscr();
    const auto stack = alternate_signal_stacks_.find(processor);
    const auto on_stack = stack != alternate_signal_stacks_.end() &&
        (stack->second.flags & darwin::signal::alternate_stack_on_stack);
    const auto use_alternate = !on_stack && (action[3] & on_stack_action) &&
        stack != alternate_signal_stacks_.end() &&
        !(stack->second.flags & darwin::signal::alternate_stack_disabled);
    const std::uint64_t top = use_alternate
        ? static_cast<std::uint64_t>(stack->second.address) + stack->second.size
        : cpu.registers()[13];
    const auto bad_stack = [&] {
        // sendsig's failed copyout forces default SIGILL even if it was
        // caught, ignored or masked; do not recursively enter a broken stack.
        signal_actions_[4] = { };
        exit_process(0, 4);
        cpu.halt(Dynarmic::HaltReason::UserDefined1);
    };
    if (top < sizeof(Frame) || top > std::numeric_limits<std::uint32_t>::max()) {
        bad_stack();
        return true;
    }
    const auto address = static_cast<std::uint32_t>(top - sizeof(Frame)) & ~3U;
    frame.context_address = address + offsetof(Frame, user);
    frame.user = { on_stack ? 1U : 0U, return_mask, address,
        use_alternate ? stack->second.size : 0U, on_stack ? 1U : 0U, 0,
        sizeof(MachineContext), static_cast<std::uint32_t>(address + offsetof(Frame, machine)) };
    frame.information.signal = signal;
    frame.information.address = cpu.registers()[15];
    frame.information.padding[0] = cpu.registers()[13];
    // ARM unix_signal.c distinguishes synchronous exception classes from
    // ordinary psignal metadata. No host siginfo structure enters the guest.
    switch (signal) {
    case 4: frame.information.code = 2; break; // ILL_ILLTRP
    case 8: break; // SIGFPE
    case 10: // SIGBUS / BUS_ADRALN
        frame.information.address = frame.machine.exception[2];
        frame.information.code = 1;
        break;
    case 11: // SIGSEGV / SEGV_ACCERR
        frame.information.address = frame.machine.exception[2];
        frame.information.code = 2;
        break;
    default:
        frame.information.pid = signal_sender_.pid;
        frame.information.uid = signal_sender_.uid;
        frame.information.status = signal_sender_.status;
        signal_sender_ = { };
        break;
    }
    if (!memory_.copy_in(address, std::as_bytes(std::span { &frame, 1 }))) {
        bad_stack();
        return true;
    }
    signal_state_.consume(processor, bit);
    signal_state_.resume(processor);
    static_cast<void>(signal_state_.update(processor, 3,
        handler_mask | action[2] | ((action[3] & no_defer_action) ? 0U : bit),
        DarwinSignalState::Scope::Thread));
    if ((action[3] & reset_action) && signal != 4U && signal != 5U) {
        signal_actions_[signal][0] = darwin::signal::default_action;
        signal_actions_[signal][3] &= ~(information_action | no_defer_action);
        discard_ignored_signal(signal);
    }
    if (use_alternate)
        stack->second.flags |= darwin::signal::alternate_stack_on_stack;
    cpu.registers()[0] = action[0];
    cpu.registers()[1] = (action[3] & information_action) ? flavor : 1U;
    cpu.registers()[2] = signal;
    cpu.registers()[3] = address + offsetof(Frame, information);
    cpu.registers()[13] = address;
    cpu.registers()[15] = action[1] & ~1U;
    // Clear IT state and select ARM/Thumb from the trampoline pointer. The
    // interrupted flags and instruction state remain in mcontext.
    cpu.set_cpsr(0x10U | ((action[1] & 1U) << 5U));
    cpu.clear_halt();
    pending_io_poll_cache_.erase(processor);
    refresh_pending_event_processor_locked(processor);
    return true;
}

} // namespace ilemu
