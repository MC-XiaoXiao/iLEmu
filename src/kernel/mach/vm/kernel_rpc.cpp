// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// Dispatch direct kernel RPC traps for guest virtual-memory
// operations.
//
// Apple public ABI/behavior references (guest profiles may differ):
// https://github.com/apple-oss-distributions/xnu/blob/xnu-792.24.17/osfmk/mach/vm_map.defs
// https://github.com/apple-oss-distributions/xnu/blob/xnu-1699.22.73/osfmk/mach/mach_vm.defs
// https://github.com/apple-oss-distributions/xnu/blob/xnu-1699.22.73/osfmk/kern/ipc_mig.c

#include "kernel/kernel.hpp"

#include "kernel/darwin_abi.hpp"

#include "../support.hpp"
#include "deallocate.hpp"
#include "protect.hpp"

#include <cstdint>
#include <mutex>

namespace ilemu {

using namespace mach_support;

bool CompatibilityKernel::dispatch_mach_vm_kernel_rpc_trap(
    Cpu& cpu, std::uint32_t trap)
{
    // ARM32 libsystem exports both 64-bit mach_vm_* and pointer-sized
    // vm_* direct traps. Keep their fast paths in one handler;
    // the MIG entry points remain the fallback when the target is not valid.
    if (trap != 10U && trap != 11U && trap != 12U && trap != 13U &&
        trap != 14U && trap != 15U)
        return false;

    auto& registers = cpu.registers();
    const bool wide_table = shared_state_->darwin_abi.mach_kernel_rpc ==
        DarwinMachKernelRpcAbi::DirectWideVmAndPortTraps;
    if (wide_table && (trap == 11U || trap == 13U)) {
        // The wide-only trap table (xnu-2422+) removed vm_allocate and
        // vm_deallocate: those slots are kern_invalid, which answers
        // KERN_INVALID_ARGUMENT. No firmware libsystem traps here.
        registers[0] = 4; // KERN_INVALID_ARGUMENT
        return true;
    }
    if (wide_table && trap == 15U) {
        // Slot 15 became _kernelrpc_mach_vm_map_trap. Let libsystem's native
        // slow path use the complete MIG mapping implementation.
        registers[0] = darwin::mach_message::send_invalid_destination;
        return true;
    }
    {
        std::lock_guard mach_lock { shared_state_->mach_mutex };
        const auto target = target_task_for_port(
            *shared_state_, process_.pid, registers[0]);
        if (!target || *target != process_.pid) {
            registers[0] = darwin::mach_message::send_invalid_destination;
            return true;
        }
    }

    if (trap == 10U || trap == 11U) {
        const auto address_pointer = registers[1];
        const bool wide = trap == 10U;
        const auto requested_address = wide
            ? memory_.read64(address_pointer)
            : std::optional<std::uint64_t> { memory_.read32(address_pointer) };
        const auto size = static_cast<std::uint64_t>(registers[2]) |
            (wide ? static_cast<std::uint64_t>(registers[3]) << 32U : 0U);
        const auto flags = registers[wide ? 4 : 3];
        if (!requested_address ||
            !memory_.accessible(address_pointer, wide ? 8U : 4U,
                MemoryPermission::Write)) {
            registers[0] = darwin::mach::invalid_address;
            return true;
        }

        const auto allocation = allocate_guest_vm_region(
            memory_, darwin::mach::vm_allocation::Contract { shared_state_->darwin_abi.abi_epoch },
            *requested_address, size, flags);
        if (allocation.result == darwin::mach::success &&
            !(wide ? memory_.write64(address_pointer, allocation.address)
                   : memory_.write32(address_pointer, allocation.address))) {
            static_cast<void>(unmap_memory(cpu,
                allocation.address, static_cast<std::uint32_t>(size)));
            registers[0] = darwin::mach::invalid_address;
            return true;
        }
        registers[0] = allocation.result;
        return true;
    }

    if (trap == 14U || trap == 15U) {
        const bool wide = trap == 14U;
        const auto address = std::uint64_t { registers[1] } |
            (wide ? std::uint64_t { registers[2] } << 32U : 0U);
        const auto size = wide ? std::uint64_t { registers[3] } |
            (std::uint64_t { registers[4] } << 32U) : registers[2];
        registers[0] = vm_mig::Protection::execute(memory_, cpu, address, size,
            wide, registers[wide ? 5U : 3U] != 0U, registers[wide ? 6U : 4U]);
        return true;
    }

    // XNU's direct deallocation traps are 12 and 13 across the supported
    // ARM32 variants. Treat already-unmapped pages as successful and reuse the
    // same AddressSpace operation as the MIG vm_deallocate path.
    const bool wide = trap == 12U;
    const auto address = static_cast<std::uint64_t>(registers[1]) |
        (wide ? static_cast<std::uint64_t>(registers[2]) << 32U : 0U);
    const auto size = wide
        ? static_cast<std::uint64_t>(registers[3]) |
              (static_cast<std::uint64_t>(registers[4]) << 32U)
        : registers[2];
    registers[0] = vm_mig::Deallocation::execute(memory_, cpu, address, size, wide);
    return true;
}

} // namespace ilemu
