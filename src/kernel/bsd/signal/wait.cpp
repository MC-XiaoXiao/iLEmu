// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include <kernel/kernel.hpp>
#include <kernel/darwin_abi.hpp>

#include <bit>

namespace ilemu {

void CompatibilityKernel::return_waited_signal(Cpu& cpu,
    std::uint32_t signal, std::uint32_t output)
{
    // XNU consumes the signal before copyout, including an EFAULT output.
    if (output != 0 && !memory_.write32(output, signal))
        bsd_error(cpu, darwin::error::bad_address);
    else
        bsd_success(cpu, 0);
}

void CompatibilityKernel::dispatch_bsd_signal_wait(Cpu& cpu)
{
    const auto set_address = cpu.registers()[0];
    const auto output_address = cpu.registers()[1];
    if (set_address == 0) {
        bsd_error(cpu, darwin::error::invalid_argument);
        return;
    }
    const auto set = memory_.read32(set_address);
    if (!set) {
        bsd_error(cpu, darwin::error::bad_address);
        return;
    }
    const auto signals = *set & ~DarwinSignalState::unmaskable;
    if (signals == 0) {
        bsd_error(cpu, darwin::error::invalid_argument);
        return;
    }
    const auto processor = cpu.processor_id();
    if (const auto signal = signal_state_.take_pending(processor, signals)) {
        return_waited_signal(cpu, *signal, output_address);
        return;
    }
    record_bsd_sleep();
    signal_state_.begin_wait(processor, signals);
    pending_signal_waits_[processor] = { signals, output_address, std::nullopt, std::nullopt };
    process_.waiting_for_events = true;
    cpu.halt(Dynarmic::HaltReason::UserDefined5);
}

bool CompatibilityKernel::complete_signal_wait(Cpu& cpu, bool interrupted)
{
    const auto processor = cpu.processor_id();
    const auto found = pending_signal_waits_.find(processor);
    if (found == pending_signal_waits_.end())
        return false;
    const auto pending = found->second;
    if (!pending.selected && !pending.interruption_result && !interrupted)
        return false;
    signal_state_.resume(processor);
    pending_signal_waits_.erase(found);
    const auto error = pending.interruption_result.value_or(
        interrupted ? darwin::error::interrupted : 0U);
    if (error != 0 && signal_state_.wait_reports_interrupt()) {
        bsd_error(cpu, error);
    } else {
        // 792/1228 convert EINTR to success using the saved uu_sigwait set;
        // 1456+ returns EINTR. All epochs convert ERESTART to success.
        const auto signal = pending.selected.value_or(
            static_cast<std::uint32_t>(std::countr_zero(pending.signals)) + 1U);
        signal_state_.consume(processor, 1U << (signal - 1U));
        return_waited_signal(cpu, signal, pending.output_address);
    }
    process_.waiting_for_events = false;
    cpu.clear_halt();
    return true;
}

} // namespace ilemu
