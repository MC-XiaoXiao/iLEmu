// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
#include "kernel/mach_dispatch_table.hpp"
#include "kernel/mach_route_policy.hpp"
#include "kernel/kernel.hpp"
#include <stdexcept>

namespace ilemu {
MachDispatchTable::Adapter MachDispatchTable::adapter_for(syscall_routes::Handler handler)
{
    using syscall_routes::Handler;
    switch (handler) {
    case Handler::MachInline:
        return [](CompatibilityKernel&, Cpu& cpu, std::uint32_t) {
            cpu.registers()[0] = 0; // legacy init_process
        };
    case Handler::MachMessage:
        return [](CompatibilityKernel& kernel, Cpu& cpu, std::uint32_t trap) {
            // ARM argument eight for mach_msg_overwrite_trap is in r8.
            kernel.dispatch_mach_message(cpu, trap == 32U && cpu.registers()[8] != 0U
                    ? std::optional { cpu.registers()[8] } : std::nullopt);
        };
    case Handler::MachThreadSelf:
        return [](CompatibilityKernel& kernel, Cpu& cpu, std::uint32_t) {
            kernel.dispatch_mach_thread_self_trap(cpu);
        };
    case Handler::MachVmRpc:
        return [](CompatibilityKernel& kernel, Cpu& cpu, std::uint32_t trap) {
            static_cast<void>(kernel.dispatch_mach_vm_kernel_rpc_trap(cpu, trap));
        };
    case Handler::MachPortRpc:
        return [](CompatibilityKernel& kernel, Cpu& cpu, std::uint32_t trap) {
            static_cast<void>(kernel.dispatch_mach_port_kernel_rpc_trap(cpu, trap));
        };
    case Handler::MachClock:
        return [](CompatibilityKernel& kernel, Cpu& cpu, std::uint32_t trap) {
            kernel.dispatch_mach_clock_trap(cpu, trap);
        };
    case Handler::MachSemaphore:
        return [](CompatibilityKernel& kernel, Cpu& cpu, std::uint32_t trap) {
            kernel.dispatch_mach_semaphore_trap(cpu, trap);
        };
    case Handler::MachTask:
        return [](CompatibilityKernel& kernel, Cpu& cpu, std::uint32_t trap) {
            kernel.dispatch_mach_task_trap(cpu, trap);
        };
    case Handler::MachScheduler:
        return [](CompatibilityKernel& kernel, Cpu& cpu, std::uint32_t trap) {
            kernel.dispatch_mach_scheduler_trap(cpu, trap);
        };
    case Handler::MachTimer:
        return [](CompatibilityKernel& kernel, Cpu& cpu, std::uint32_t trap) {
            kernel.dispatch_mach_timer_trap(cpu, trap);
        };
    default:
        throw std::invalid_argument("Mach catalog handler has no executable adapter");
    }
}
MachDispatchTable::MachDispatchTable(const DarwinAbi& abi, std::string_view abi_profile)
{
    using namespace syscall_routes;
    const auto catalog = build(abi);
    const MachRoutePolicy policy { abi_profile };
    for (std::uint32_t trap = 0; trap < bindings_.size(); ++trap) {
        const auto* entry = catalog.find(Domain::MachTrap, trap);
        const auto resolved = policy.resolve(entry, trap);
        bindings_[trap] = { resolved.invokes_handler() ? adapter_for(entry->handler) : nullptr,
            resolved.fallback_result, resolved.trace_unknown };
    }
}
void MachDispatchTable::dispatch(CompatibilityKernel& kernel, Cpu& cpu, std::uint32_t trap) const
{
    const Binding fallback;
    const auto& binding = trap < bindings_.size() ? bindings_[trap] : fallback;
    if (binding.adapter) {
        binding.adapter(kernel, cpu, trap);
        return;
    }
    if (binding.trace_unknown)
        kernel.trace_unknown(cpu, "Mach trap", trap);
    cpu.registers()[0] = binding.result;
}
} // namespace ilemu
