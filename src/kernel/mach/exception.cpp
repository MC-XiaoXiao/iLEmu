// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
#include "foundation/darwin_address_layout.hpp"
#include "kernel/darwin_abi.hpp"
#include "kernel/darwin_signal_context.hpp"
#include "kernel/kernel.hpp"
#include <mutex>

namespace ilemu {
bool CompatibilityKernel::handle_cpu_exception(
    Cpu& cpu, const CpuRunResult& result)
{
    if (!result.fault && !result.architectural_exception)
        return false;
    const std::lock_guard lock { mutex_ };
    MachExceptionDelivery::Exception exception;
    if (result.fault) {
        const auto& fault = *result.fault;
        const bool protection = memory_.mapped(fault.address);
        const bool execute = fault.access == MemoryPermission::Execute;
        const bool write = fault.access == MemoryPermission::Write;
        exception.type = 1; // EXC_BAD_ACCESS
        exception.codes = { protection ? 2U : 1U, fault.address };
        exception.signal =
            protection ? 10U : 11U; // ux_exception: SIGBUS / SIGSEGV
        // ARM short-descriptor page faults (proc_reg.h): translation=7,
        // permission=15, WnR=bit11. Guest memory is managed in 4KiB pages.
        exception.arm_state = { execute ? 3U : 4U,
            (protection ? 15U : 7U) | (write ? 1U << 11U : 0U), fault.address };
    } else {
        const auto& fault = *result.architectural_exception;
        const bool breakpoint = fault.kind == CpuException::Kind::Breakpoint;
        exception.type = breakpoint ? 6U : 2U;
        exception.signal = breakpoint ? 5U : 4U;
        exception.codes[0] = 1; // EXC_ARM_BREAKPOINT / EXC_ARM_UNDEFINED
        if (cpu.cpsr() & 0x20U) {
            const auto first = memory_.read16(fault.pc).value_or(0);
            const bool wide = (first & 0xf800U) >= 0xe800U;
            exception.codes[1] =
                wide ? (static_cast<std::uint32_t>(first) << 16U) |
                           memory_.read16(fault.pc + 2U).value_or(0)
                     : first;
        } else
            exception.codes[1] = memory_.read32(fault.pc).value_or(0);
        exception.arm_state = { 1U, 0U, 0U }; // T_UNDEF
    }
    complete_cpu_exception(cpu,
        exception_delivery_.begin(*shared_state_, process_, cpu, exception));
    return true;
}

bool CompatibilityKernel::complete_cpu_exception(
    Cpu& cpu, const MachExceptionDelivery::Completion& completion)
{
    const auto processor = cpu.processor_id();
    if (completion.outcome == MachExceptionDelivery::Outcome::Waiting) {
        cpu.halt(Dynarmic::HaltReason::UserDefined2);
        refresh_pending_event_processor_locked(processor);
        process_.waiting_for_events = true;
        return false;
    }
    cpu.clear_halt();
    if (completion.outcome == MachExceptionDelivery::Outcome::Unhandled) {
        auto exception = completion.exception;
        // ux_exception.c treats a protection fault in the reserved main
        // stack as overflow. Only a ready SA_ONSTACK disposition may catch
        // it; otherwise XNU forces an unmasked default SIGSEGV.
        const auto stack_top =
            darwin_address_bounds(shared_state_->darwin_abi.address_layout)
                .stack_top;
        if (exception.type == 1U && exception.codes[0] == 2U &&
            exception.codes[1] >= stack_top - initial_user_stack_size &&
            exception.codes[1] < stack_top) {
            exception.signal = 11U;
            constexpr auto bit = 1U << 10U;
            auto& action = signal_actions_[exception.signal];
            if (action[0] == darwin::signal::ignore_action ||
                (signal_state_.mask(processor) & bit) ||
                (signal_state_.waiting_for(processor) & bit) ||
                !(action[3] & darwin::signal_context::on_stack_action)) {
                action[0] = darwin::signal::default_action;
                if (signal_state_.waiting_for(processor) & bit)
                    signal_state_.resume(processor);
                static_cast<void>(signal_state_.update(
                    processor, 2U, bit, DarwinSignalState::Scope::Thread));
            }
        }
        if (signal_actions_[exception.signal][0] >
            darwin::signal::ignore_action)
            synchronous_exceptions_.insert_or_assign(processor, exception);
        static_cast<void>(
            deliver_signal_to_thread(exception.signal, processor, 0, 0));
        if (!process_.exited)
            static_cast<void>(deliver_pending_signal(cpu));
    }
    refresh_pending_event_processor_locked(processor);
    process_.waiting_for_events =
        !pending_event_processors_.empty() || !pending_waits_.empty();
    if (process_.exited)
        cpu.halt(Dynarmic::HaltReason::UserDefined1);
    return true;
}
} // namespace ilemu
