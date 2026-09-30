// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "kernel/kernel.hpp"
#include "kernel/darwin_signal_context.hpp"
#include <algorithm>
#include <span>

namespace ilemu {

void CompatibilityKernel::dispatch_bsd_signal_return(Cpu& cpu)
{
    using namespace darwin::signal_context;
    const auto processor = cpu.processor_id();
    const auto style = cpu.registers()[1];
    const auto set_on_stack = [&](bool enabled) {
        auto it = alternate_signal_stacks_.find(processor);
        if (it == alternate_signal_stacks_.end()) {
            if (!enabled)
                return;
            it = alternate_signal_stacks_.emplace(processor,
                AlternateSignalStack { 0, 0,
                    darwin::signal::alternate_stack_disabled }).first;
        }
        if (enabled)
            it->second.flags |= darwin::signal::alternate_stack_on_stack;
        else
            it->second.flags &= ~darwin::signal::alternate_stack_on_stack;
    };
    if (style == set_alternate_stack || style == reset_alternate_stack) {
        set_on_stack(style == set_alternate_stack);
        bsd_success(cpu, 0);
        return;
    }

    UserContext user { };
    if (!memory_.copy_out(cpu.registers()[0],
            std::as_writable_bytes(std::span { &user, 1 }))) {
        bsd_error(cpu, darwin::error::bad_address);
        return;
    }
    if (user.machine_size != sizeof(MachineContext)) {
        bsd_error(cpu, darwin::error::invalid_argument);
        return;
    }
    MachineContext machine { };
    if (!memory_.copy_out(user.machine_address,
            std::as_writable_bytes(std::span { &machine, 1 }))) {
        bsd_error(cpu, darwin::error::bad_address);
        return;
    }

    // Finish both copyins before changing registers, the mask or altstack.
    // The exception state is informational; sigreturn restores general/VFP
    // state only, leaving the thread's TLS identity intact.
    const auto cpsr = restored_cpsr(machine.general[darwin::arm_thread::cpsr_index],
        cpu.cpsr());
    set_on_stack((user.on_stack & 1U) != 0);
    static_cast<void>(signal_state_.update(processor, 3, user.signal_mask,
        DarwinSignalState::Scope::Thread));
    std::copy_n(machine.general.begin(), cpu.registers().size(),
        cpu.registers().begin());
    cpu.extension_registers() = machine.floating;
    cpu.set_fpscr(machine.fpscr);
    cpu.set_cpsr(cpsr);
    // XNU EJUSTRETURN: do not publish a normal syscall result or clear carry.
}

} // namespace ilemu
