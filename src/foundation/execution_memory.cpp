/* SPDX-License-Identifier: MPL-2.0 */
#include "foundation/execution_memory.hpp"
#include "foundation/address_space.hpp"
#include "foundation/arm_cpu_model.hpp"
#include <array>
#include <limits>
#include <map>
#include <stdexcept>
#include <utility>

namespace ilemu {
namespace {
    constexpr std::uint32_t page_base(std::uint32_t address)
    {
        return address & ~(AddressSpace::page_size - 1U);
    }
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
class ExecutionMemory::Impl {
public:
    struct View {
        std::shared_ptr<const execution::CodeIdentity> identity =
            std::make_shared<execution::CodeIdentity>();
        std::uint64_t epoch = 1, mapping = 0, first = 0, second = 0;
        bool initialized = false, has_second = false;
    };
    class Lease final : public execution::InstructionLease {
        struct Page {
            std::uint32_t base = 0;
            std::shared_ptr<GuestPageBacking> backing;
            std::shared_lock<std::shared_mutex> lock;
            std::uint64_t generation() const noexcept
            {
                return backing ? backing->shared_write_generation() : 0;
            }
        };

    public:
        Lease(Impl& owner, View& view, std::uint32_t pc, bool thumb)
            : owner_(owner)
            , memory_(owner.memory_)
            , view_(view)
            , pc_(pc)
            , thumb_(thumb)
        {
            owner_.memory_.synchronize_shared_write_tracking();
            owner_.active_ = this;
        }
        ~Lease() override { owner_.active_ = nullptr; }
        std::shared_ptr<const execution::CodeIdentity>
        identity() const noexcept override
        {
            return view_.identity;
        }
        std::optional<std::uint64_t> generation() const noexcept override
        {
            if (count_ == 0)
                return std::nullopt;
            const auto mapping = owner_.memory_.executable_content_generation();
            const auto first = pages_[0].generation();
            const bool changed =
                view_.initialized &&
                (mapping != view_.mapping || first != view_.first ||
                    (count_ == 2 && view_.has_second &&
                        pages_[1].generation() != view_.second));
            if (changed) {
                if (view_.epoch == std::numeric_limits<std::uint64_t>::max())
                    return std::nullopt;
                ++view_.epoch;
                view_.has_second = false;
            }
            view_.initialized = true;
            view_.mapping = mapping;
            view_.first = first;
            if (count_ == 2) {
                view_.has_second = true;
                view_.second = pages_[1].generation();
            }
            return view_.epoch;
        }
        bool allows(std::uint32_t address, unsigned size) const noexcept
        {
            if (std::uint64_t { address } + size > (std::uint64_t { 1 } << 32U))
                return false;
            for (unsigned byte = 0; byte < size; ++byte) {
                const auto base = page_base(address + byte);
                bool found = false;
                for (unsigned i = 0; i < count_; ++i)
                    found |= pages_[i].base == base;
                if (!found)
                    return false;
            }
            return true;
        }
        std::optional<execution::MemoryFault> prepare(
            std::uint32_t address, unsigned size)
        {
            if ((address != pc_ || size != (thumb_ ? 2U : 4U)) &&
                (!thumb_ || address != pc_ + 2U || size != 2U))
                throw std::logic_error(
                    "fetch preparation outside entry instruction");
            TaskVmEvents::Scope actor { owner_.memory_.task_vm_events() };
            if (!owner_.memory_.prepare_instruction_fetch(address, size))
                return owner_.fault(address, size, MemoryPermission::Execute);
            const auto base = page_base(address);
            for (unsigned i = 0; i < count_; ++i)
                if (pages_[i].base == base)
                    return { };
            if (count_ == pages_.size())
                throw std::logic_error("instruction lease exceeds two pages");
            auto backing = owner_.memory_.instruction_page_backing(address);
            if (!backing)
                return owner_.fault(address, size, MemoryPermission::Execute);
            auto& page = pages_[count_];
            page.base = base;
            page.backing = std::move(*backing);
            bool duplicate = false;
            for (unsigned i = 0; i < count_; ++i)
                duplicate |= page.backing == pages_[i].backing;
            if (page.backing && !duplicate)
                page.lock = page.backing->lock_instruction_read();
            ++count_;
            return { };
        }
        bool thumb() const noexcept { return thumb_; }

    private:
        Impl& owner_;
        AddressSpace::ExclusiveAccess memory_;
        View& view_;
        std::uint32_t pc_;
        bool thumb_;
        std::array<Page, 2> pages_;
        unsigned count_ = 0;
    };
    Impl(AddressSpace& memory, const ArmCpuModel& model,
        ExecutionMemoryPolicy policy, std::size_t maximum_views)
        : memory_(memory)
        , model_(model)
        , policy_(policy)
        , maximum_views_(maximum_views)
    {
        if (maximum_views == 0 ||
            (policy.pc_store_offset != 8 && policy.pc_store_offset != 12))
            throw std::invalid_argument("invalid execution memory policy");
    }
    execution::MemoryFault fault(
        std::uint32_t address, unsigned size, MemoryPermission access)
    {
        last_fault_ = ilemu::MemoryFault { address, size, access,
            "unmapped address or protection failure" };
        return { address, (memory_.mapped(address, size) ? 13U : 5U) |
                              (access == MemoryPermission::Write
                                      ? 1U << 11U : 0U) };
    }
    void require_checked() const
    {
        if (active_)
            throw std::logic_error(
                "checked access requires releasing the code lease");
    }
    AddressSpace& memory_;
    const ArmCpuModel& model_;
    ExecutionMemoryPolicy policy_;
    std::size_t maximum_views_;
    std::map<std::uint32_t, View> views_;
    Lease* active_ = nullptr;
    WriteObserver write_observer_;
    std::optional<ilemu::MemoryFault> last_fault_;
};
ExecutionMemory::ExecutionMemory(AddressSpace& memory, const ArmCpuModel& model,
    ExecutionMemoryPolicy policy, std::size_t maximum_views)
    : impl_(std::make_unique<Impl>(memory, model, policy, maximum_views))
{
}
ExecutionMemory::~ExecutionMemory() = default;
std::unique_ptr<execution::InstructionLease>
ExecutionMemory::acquire_code_lease(std::uint32_t pc, bool thumb)
{
    if (impl_->active_)
        throw std::logic_error("execution memory already leased");
    const auto base = page_base(pc);
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
    const auto base = page_base(address);
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
