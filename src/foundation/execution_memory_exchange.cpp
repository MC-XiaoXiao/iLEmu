/* SPDX-License-Identifier: MPL-2.0 */
#include "execution_memory_impl.hpp"
namespace ilemu {
std::optional<execution::MemoryRead> ExecutionMemory::exchange(
    std::uint32_t address, execution::AccessSize size, std::uint32_t value)
{
    impl_->require_checked();
    const auto width = static_cast<unsigned>(size);
    if ((width != 1 && width != 4) || (address & (width - 1U)) != 0)
        throw std::invalid_argument("invalid atomic exchange width/alignment");
    TaskVmEvents::Scope actor { impl_->memory_.task_vm_events() };
    std::optional<std::uint32_t> previous;
    {
        // Keep mapping/permission diagnosis in the same bounded transaction.
        // exchange8/32 also lock physical shared backing and account writes,
        // COW/code invalidation and outstanding exclusive reservations.
        AddressSpace::ExclusiveAccess access { impl_->memory_ };
        previous = width == 1
            ? std::optional<std::uint32_t> { impl_->memory_.exchange8(
                  address, static_cast<std::uint8_t>(value)) }
            : impl_->memory_.exchange32(address, value);
        if (!previous) {
            const auto permission = impl_->memory_.accessible(address, width,
                                        MemoryPermission::Read)
                ? MemoryPermission::Write : MemoryPermission::Read;
            return execution::MemoryRead { 0,
                impl_->fault(address, width, permission) };
        }
    }
    if (impl_->write_observer_)
        impl_->write_observer_(address, width, width == 1 ? value & 255U : value);
    return execution::MemoryRead { *previous, { } };
}
}
