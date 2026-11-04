/* SPDX-License-Identifier: MPL-2.0 */
#include "kernel/kernel.hpp"
#include "kernel/darwin_abi.hpp"

namespace ilemu {

bool CompatibilityKernel::dispatch_bsd_process_role(Cpu& cpu, std::uint32_t number)
{
    const auto& args = cpu.registers();
    if ((number != 96U && number != darwin::syscall::get_priority) ||
        args[0] != ProcessPriorityProfile::darwin_role_scope)
        return false;

    const auto profile = ProcessPriorityProfile::for_version(shared_state_->darwin_version);
    if (!profile.darwin_roles) {
        bsd_error(cpu, darwin::error::invalid_argument);
        return true;
    }
    const auto pid = args[1] == 0U ? process_.pid : args[1];
    std::lock_guard lock { shared_state_->mach_mutex };
    const auto target = shared_state_->processes.find(pid);
    if (target == shared_state_->processes.end() || target->second.exited) {
        bsd_error(cpu, darwin::error::no_such_process);
        return true;
    }
    auto& policy = *target->second.execution_policy;
    if (number == darwin::syscall::get_priority) {
        bsd_success(cpu, static_cast<std::uint32_t>(policy.role()));
        return true;
    }
    const auto role = static_cast<std::int32_t>(args[2]);
    if (role < 0 || role > profile.maximum_role) {
        bsd_error(cpu, darwin::error::invalid_argument);
        return true;
    }
    const auto& owner = target->second;
    if (process_.uid != 0U && process_.effective_uid != 0U &&
        process_.uid != owner.uid && process_.uid != owner.effective_uid &&
        process_.effective_uid != owner.uid && process_.effective_uid != owner.effective_uid) {
        bsd_error(cpu, darwin::error::operation_not_permitted);
        return true;
    }
    policy.set_role(static_cast<DarwinProcessRole>(role));
    bsd_success(cpu, 0U);
    return true;
}

} // namespace ilemu
