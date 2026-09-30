// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// Dispatch direct kernel RPC traps for Mach port operations.
//
// Apple public ABI/behavior references (guest profiles may differ):
// https://github.com/apple-oss-distributions/xnu/blob/xnu-792.24.17/osfmk/mach/mach_port.defs
// https://github.com/apple-oss-distributions/xnu/blob/xnu-1699.22.73/osfmk/kern/ipc_mig.c

#include "kernel/kernel.hpp"

#include "kernel/darwin_abi.hpp"

#include "../support.hpp"
#include "guarded.hpp"
#include "lifecycle.hpp"

#include <mutex>

namespace ilemu {

using namespace mach_support;

bool CompatibilityKernel::dispatch_mach_port_kernel_rpc_trap(
    Cpu& cpu, std::uint32_t trap)
{
    const bool guarded = shared_state_->darwin_abi.mach_kernel_rpc ==
            DarwinMachKernelRpcAbi::DirectWideVmAndPortTraps &&
        (trap == 24U || trap == 25U || trap == 41U || trap == 42U);
    if (!guarded && trap != 16U && trap != 17U && trap != 18U && trap != 19U &&
        trap != 20U && trap != 21U && trap != 22U && trap != 23U) {
        return false;
    }

    auto& registers = cpu.registers();
    std::lock_guard mach_lock { shared_state_->mach_mutex };
    const auto target = target_task_for_port(
        *shared_state_, process_.pid, registers[0]);
    if (guarded) {
        registers[0] = dispatch_guarded_port_trap(*shared_state_, memory_,
            target && *target == process_.pid ? *target : 0U, registers, trap);
        return true;
    }
    if (!target || *target != process_.pid) {
        registers[0] = darwin::mach_message::send_invalid_destination;
        return true;
    }

    if (trap == 16U) { // _kernelrpc_mach_port_allocate_trap
        const auto right = registers[1];
        const auto output_address = registers[2];
        const auto allocated = port_mig::Lifecycle::allocate_locked(
            *shared_state_, *target, right);
        if (allocated.error != 0U) {
            registers[0] = allocated.error;
            return true;
        }
        if (!memory_.write32(output_address, allocated.name)) {
            static_cast<void>(
                destroy_port_name_locked(*shared_state_, *target, allocated.name));
            registers[0] = darwin::mach::invalid_address;
            return true;
        }
        registers[0] = darwin::mach::success;
        return true;
    }

    if (trap == 17U) { // _kernelrpc_mach_port_destroy_trap
        const auto name = registers[1];
        registers[0] =
            name == xnu::ipc::null_name || name == xnu::ipc::dead_name
                ? darwin::mach::success
            : destroy_port_name_locked(*shared_state_, *target, name)
                ? darwin::mach::success
                : darwin::mach::invalid_name;
        return true;
    }

    if (trap == 19U) { // _kernelrpc_mach_port_mod_refs_trap
        const auto right = registers[2];
        registers[0] =
            right > static_cast<std::uint32_t>(xnu::ipc::Right::DeadName)
                ? darwin::mach::invalid_value
                : modify_port_references_locked(*shared_state_, *target,
                      registers[1], static_cast<xnu::ipc::Right>(right),
                      static_cast<std::int32_t>(registers[3]));
        return true;
    }

    if (trap == 21U) { // _kernelrpc_mach_port_insert_right_trap
        registers[0] = insert_port_right_locked(*shared_state_, process_.pid,
            *target, registers[1], registers[2], registers[3]);
        return true;
    }

    if (trap == 20U || trap == 22U || trap == 23U) {
        // _kernelrpc_mach_port_{move,insert,extract}_member_trap share the
        // task/member/port-set register contract.
        const auto operation =
            trap == 20U ? PortMembershipOperation::Move
            : trap == 22U ? PortMembershipOperation::Insert
                          : PortMembershipOperation::Extract;
        const auto membership = modify_port_membership_locked(*shared_state_, *target,
            registers[1], registers[2], operation);
        registers[0] = membership.result;
        return true;
    }

    registers[0] = port_mig::Lifecycle::deallocate_locked(
        *shared_state_, *target, registers[1]);
    return true;
}

} // namespace ilemu
