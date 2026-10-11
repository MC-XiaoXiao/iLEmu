/* SPDX-License-Identifier: MPL-2.0 */
#include "execution_memory_impl.hpp"
#include "foundation/address_space.hpp"
#include "foundation/arm_cpu_model.hpp"
#include <array>
#include <limits>
#include <map>
#include <stdexcept>
#include <utility>

namespace ilemu {
namespace {
    unsigned access_width(execution::AccessSize size)
    {
        switch (size) {
        case execution::AccessSize::Byte:
        case execution::AccessSize::Half:
        case execution::AccessSize::Word:
            return static_cast<unsigned>(size);
        }
        throw std::invalid_argument("invalid guest access width");
    }
}
ExecutionMemory::ExecutionMemory(AddressSpace& memory, const ArmCpuModel& model,
    ExecutionMemoryPolicy policy, std::size_t maximum_views)
    : impl_(std::make_shared<Impl>(memory, model, policy, maximum_views))
{
}
ExecutionMemory::~ExecutionMemory() = default;
execution::ThumbArithmeticProfile
ExecutionMemory::thumb_arithmetic_profile() const noexcept
{
    return impl_->model_.unpredictable_instruction_policy() ==
                   ArmUnpredictableInstructionPolicy::CortexA8
               ? execution::ThumbArithmeticProfile::GeneralPurposeStackPointer
               : execution::ThumbArithmeticProfile::Architectural;
}
execution::VfpRegisterProfile
ExecutionMemory::vfp_register_profile() const noexcept
{
    return impl_->model_.kind() == ArmCpuModelKind::Arm1176JzfS
               ? execution::VfpRegisterProfile::D16
               : execution::VfpRegisterProfile::D32;
}
execution::ArmInstructionProfile
ExecutionMemory::arm_instruction_profile() const noexcept
{
    return impl_->model_.architecture_version() == ArmArchitectureVersion::Armv6K
               ? execution::ArmInstructionProfile::Armv6
               : execution::ArmInstructionProfile::Armv6T2;
}
execution::AluPcWriteProfile
ExecutionMemory::alu_pc_write_profile() const noexcept
{
    return impl_->model_.architecture_version() == ArmArchitectureVersion::Armv7
               ? execution::AluPcWriteProfile::Interworking
               : execution::AluPcWriteProfile::Branch;
}
execution::FloatingExceptionProfile
ExecutionMemory::floating_exception_profile() const noexcept
{
    return impl_->model_.kind() == ArmCpuModelKind::Arm1176JzfS
               ? execution::FloatingExceptionProfile::TrapEnables
               : execution::FloatingExceptionProfile::Untrapped;
}
std::unique_ptr<execution::InstructionLease>
ExecutionMemory::acquire_code_lease(std::uint32_t pc, bool thumb)
{
    if (impl_->active_)
        throw std::logic_error("execution memory already leased");
    const auto base = execution_page_base(pc);
    if (!impl_->views_.contains(base) &&
        impl_->views_.size() == impl_->maximum_views_)
        impl_->views_.clear();
    auto& view = impl_->views_.try_emplace(base).first->second;
    return std::make_unique<Impl::Lease>(*impl_, view, pc, thumb);
}
std::optional<execution::MemoryFault>
ExecutionMemory::prepare_instruction_fetch(std::uint32_t address, unsigned size)
{
    if (!impl_->active_)
        throw std::logic_error("fetch preparation requires a code lease");
    return impl_->active_->prepare(address, size);
}
execution::InstructionRegion ExecutionMemory::instruction_region(
    std::uint32_t address) const
{
    const auto base = execution_page_base(address);
    return { base, std::uint64_t { base } + AddressSpace::page_size };
}
std::optional<std::uint16_t> ExecutionMemory::fetch16(std::uint32_t address)
{
    if (!impl_->active_ || !impl_->active_->allows(address, 2))
        return { };
    TaskVmEvents::Scope inspection { nullptr };
    return impl_->memory_.read16(address, MemoryPermission::Execute);
}
std::optional<std::uint32_t> ExecutionMemory::fetch32(std::uint32_t address)
{
    if (!impl_->active_ || !impl_->active_->allows(address, 4))
        return { };
    TaskVmEvents::Scope inspection { nullptr };
    return impl_->memory_.read32(address, MemoryPermission::Execute);
}
std::uint64_t ExecutionMemory::ticks_for_instruction(
    std::uint32_t address, std::uint32_t word) const
{
    if (!impl_->active_)
        throw std::logic_error("instruction timing requires a code lease");
    return impl_->model_.ticks_for_instruction(
        impl_->active_->thumb(), address, word);
}
execution::DirectMemory ExecutionMemory::direct_memory()
{
    if (!impl_->active_)
        throw std::logic_error("direct memory requires a code lease");
    return { impl_->memory_.jit_read_page_table(),
        impl_->write_observer_ ? nullptr : impl_->memory_.jit_write_page_table(),
        impl_->policy_.permits_unaligned ? 1U : 0U,
        impl_->policy_.pc_store_offset };
}
execution::MemoryRead ExecutionMemory::read(
    std::uint32_t address, execution::AccessSize size)
{
    impl_->require_checked();
    const auto width = access_width(size);
    if (!impl_->policy_.permits_unaligned && (address & (width - 1U)) != 0) {
        impl_->last_fault_ = ilemu::MemoryFault { address, width,
            MemoryPermission::Read, "unaligned address" };
        return { 0, execution::MemoryFault { address, 1 } };
    }
    TaskVmEvents::Scope actor { impl_->memory_.task_vm_events() };
    std::optional<std::uint32_t> value;
    switch (size) {
    case execution::AccessSize::Byte:
        value = impl_->memory_.read8(address);
        break;
    case execution::AccessSize::Half:
        value = impl_->memory_.read16(address);
        break;
    case execution::AccessSize::Word:
        value = impl_->memory_.read32(address);
        break;
    }
    return value ? execution::MemoryRead { *value, { } }
                 : execution::MemoryRead { 0,
                       impl_->fault(address, width, MemoryPermission::Read) };
}
std::optional<execution::MemoryFault> ExecutionMemory::write(
    std::uint32_t address, execution::AccessSize size, std::uint32_t value)
{
    impl_->require_checked();
    const auto width = access_width(size);
    if (!impl_->policy_.permits_unaligned && (address & (width - 1U)) != 0) {
        impl_->last_fault_ = ilemu::MemoryFault { address, width,
            MemoryPermission::Write, "unaligned address" };
        return execution::MemoryFault { address, 1U | (1U << 11U) };
    }
    TaskVmEvents::Scope actor { impl_->memory_.task_vm_events() };
    bool written = false;
    switch (size) {
    case execution::AccessSize::Byte:
        written =
            impl_->memory_.write8(address, static_cast<std::uint8_t>(value));
        break;
    case execution::AccessSize::Half:
        written =
            impl_->memory_.write16(address, static_cast<std::uint16_t>(value));
        break;
    case execution::AccessSize::Word:
        written = impl_->memory_.write32(address, value);
        break;
    }
    if (written && impl_->write_observer_)
        impl_->write_observer_(address, width, value);
    return written ? std::nullopt
                   : std::optional { impl_->fault(
                         address, width, MemoryPermission::Write) };
}
void ExecutionMemory::set_write_observer(WriteObserver observer)
{
    impl_->require_checked();
    impl_->write_observer_ = std::move(observer);
}
std::optional<ilemu::MemoryFault> ExecutionMemory::take_fault()
{
    return std::exchange(impl_->last_fault_, std::nullopt);
}
std::size_t ExecutionMemory::instruction_view_count() const noexcept
{
    return impl_->views_.size();
}
}
