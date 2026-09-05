#include "ilemu/kernel.hpp"

#include "ilemu/darwin_abi.hpp"

#include "../support.hpp"

#include <cstdint>
#include <mutex>

namespace ilemu {

using namespace mach_support;

namespace {

    MemoryPermission memory_permissions(std::uint32_t protection)
    {
        MemoryPermission result = MemoryPermission::None;
        if ((protection & 1U) != 0)
            result |= MemoryPermission::Read;
        if ((protection & 2U) != 0)
            result |= MemoryPermission::Write;
        if ((protection & 4U) != 0)
            result |= MemoryPermission::Execute;
        return result;
    }

} // namespace

bool CompatibilityKernel::dispatch_mach_vm_kernel_rpc_trap(
    Cpu& cpu, std::uint32_t trap)
{
    // iPhoneOS 5's ARM32 libsystem exports both mach_vm_* and pointer-sized
    // vm_* direct traps. Keep the fast-path ABI in one profile-gated handler;
    // the MIG entry points remain the fallback when the target is not valid.
    if (trap != 11U && trap != 13U && trap != 14U && trap != 15U)
        return false;

    auto& registers = cpu.registers();
    {
        std::lock_guard mach_lock { shared_state_->mach_mutex };
        const auto target = target_task_for_port(
            *shared_state_, process_.pid, registers[0]);
        if (!target || *target != process_.pid) {
            registers[0] = darwin::mach_message::send_invalid_destination;
            return true;
        }
    }

    if (trap == 11U) {
        const auto address_pointer = registers[1];
        const auto requested_address = memory_.read32(address_pointer);
        if (!requested_address ||
            !memory_.accessible(address_pointer, sizeof(std::uint32_t),
                MemoryPermission::Write)) {
            registers[0] = darwin::mach::invalid_address;
            return true;
        }

        const auto allocation = allocate_guest_vm_region(
            memory_, *requested_address, registers[2], registers[3]);
        if (allocation.result == darwin::mach::success &&
            !memory_.write32(address_pointer, allocation.address)) {
            static_cast<void>(memory_.unmap(allocation.address, registers[2]));
            registers[0] = darwin::mach::invalid_address;
            return true;
        }
        registers[0] = allocation.result;
        return true;
    }

    if (trap == 14U) {
        // _kernelrpc_mach_vm_protect_trap uses two 64-bit arguments in the
        // ARM32 register image: address r1:r2 and size r3:r4, followed by
        // set_maximum and new_protection in r5/r6.  The guest address space is
        // 32-bit, so reject values that cannot be represented before touching
        // the mapping.
        const auto address = static_cast<std::uint64_t>(registers[1]) |
                             (static_cast<std::uint64_t>(registers[2]) << 32U);
        const auto size = static_cast<std::uint64_t>(registers[3]) |
                          (static_cast<std::uint64_t>(registers[4]) << 32U);
        const auto result =
            address <= UINT32_MAX && size <= UINT32_MAX &&
                    protect_memory(cpu, static_cast<std::uint32_t>(address),
                        static_cast<std::uint32_t>(size),
                        memory_permissions(registers[6]))
                ? darwin::mach::success
                : darwin::mach::invalid_address;
        registers[0] = result;
        return true;
    }

    if (trap == 15U) {
        // The firmware's map fast path supplies a pointer-sized address and
        // size in r1/r2.  Its fixed-width ARM32 trampoline leaves the optional
        // mask in r3 and keeps the anywhere/protection words in the preserved
        // argument registers.  Reuse the same allocator as vm_allocate; this
        // is the null-memory-object path for which XNU publishes this trap.
        const auto address_pointer = registers[1];
        const auto requested_address = memory_.read32(address_pointer);
        const auto allocation = requested_address
                                    ? allocate_guest_vm_region(memory_,
                                          *requested_address, registers[2],
                                          registers[4], registers[3])
                                    : VmAllocationResult {
                                          darwin::mach::invalid_address, 0U };
        if (allocation.result == darwin::mach::success &&
            !memory_.write32(address_pointer, allocation.address)) {
            static_cast<void>(memory_.unmap(allocation.address, registers[2]));
            registers[0] = darwin::mach::invalid_address;
            return true;
        }
        registers[0] = allocation.result;
        return true;
    }

    // XNU treats already-unmapped pages as a successful deallocation. Reuse
    // the same AddressSpace operation as the MIG vm_deallocate path.
    static_cast<void>(memory_.unmap(registers[1], registers[2]));
    registers[0] = darwin::mach::success;
    return true;
}

} // namespace ilemu
