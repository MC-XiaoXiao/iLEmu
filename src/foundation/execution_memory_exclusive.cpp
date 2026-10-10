/* SPDX-License-Identifier: MPL-2.0 */
#include "execution_memory_impl.hpp"
#include <utility>

namespace ilemu {
class ExecutionMemory::Impl::Monitor final : public execution::ExclusiveMonitor {
public:
    Monitor(const std::shared_ptr<Impl>& owner, const execution::MemoryAccess& binding)
        : next_(owner->monitors_), owner_(owner), binding_(&binding)
    { owner->monitors_ = this; }
    ~Monitor() override
    {
        if (auto owner = owner_.lock()) {
            auto** link = &owner->monitors_;
            while (*link && *link != this)
                link = &(*link)->next_;
            if (*link)
                *link = next_;
            try { retire(*owner); } catch (...) { }
        }
    }
    bool bound_to(const execution::MemoryAccess& memory) const noexcept override
    { return !owner_.expired() && binding_ == &memory; }
    execution::ExclusiveRead load(std::uint32_t address, unsigned size) override
    {
        auto owner = owner_.lock();
        if (!owner)
            return { 0, execution::MemoryFault { address, 5 } };
        owner->require_checked();
        retire(*owner);
        if (auto fault = alignment(*owner, address, size, false))
            return { 0, fault };
        TaskVmEvents::Scope actor { owner->memory_.task_vm_events() };
        reservation_ = owner->memory_.load_exclusive(address, size);
        address_ = address;
        return reservation_
            ? execution::ExclusiveRead { reservation_->value, { } }
            : execution::ExclusiveRead { 0,
                owner->fault(address, size, MemoryPermission::Read) };
    }
    execution::ExclusiveWrite store(std::uint32_t address, unsigned size,
        std::uint64_t value) override
    {
        auto owner = owner_.lock();
        if (!owner)
            return { false, execution::MemoryFault { address, 5U | (1U << 11U) } };
        owner->require_checked();
        try {
            auto fault = alignment(*owner, address, size, true);
            std::optional<bool> written { false };
            if (!fault && reservation_) {
                TaskVmEvents::Scope actor { owner->memory_.task_vm_events() };
                written = owner->memory_.store_exclusive(address, size, value, *reservation_);
                if (!written)
                    fault = owner->fault(address, size, MemoryPermission::Write);
            }
            retire(*owner); // every attempted STREX consumes its local monitor
            if (written.value_or(false) && owner->write_observer_)
                owner->write_observer_(address, size, static_cast<std::uint32_t>(value));
            return { written.value_or(false), fault };
        } catch (...) {
            retire(*owner);
            throw;
        }
    }
    void clear() override
    {
        if (auto owner = owner_.lock()) {
            owner->require_checked();
            retire(*owner);
        }
    }
    void retire(Impl& owner)
    {
        if (auto reservation = std::exchange(reservation_, std::nullopt))
            owner.memory_.retire_exclusive(address_, *reservation);
    }
    Monitor* next_;
private:
    static std::optional<execution::MemoryFault> alignment(Impl& owner,
        std::uint32_t address, unsigned size, bool write)
    {
        if (size != 1 && size != 2 && size != 4 && size != 8)
            throw std::invalid_argument("invalid exclusive access width");
        if ((address & (size - 1U)) == 0)
            return { };
        const auto access = write ? MemoryPermission::Write : MemoryPermission::Read;
        owner.last_fault_ = ilemu::MemoryFault { address, size, access, "unaligned exclusive address" };
        return execution::MemoryFault { address, 1U | (write ? 1U << 11U : 0U) };
    }
    std::weak_ptr<Impl> owner_;
    const execution::MemoryAccess* binding_;
    std::optional<execution::ExclusiveReservation> reservation_;
    std::uint32_t address_ = 0;
};
ExecutionMemory::Impl::~Impl()
{
    // A source may be destroyed before its executor. Retire all guards while
    // the adapter's AddressSpace reference is valid; contexts retain only weak
    // ownership and reject subsequent use. No allocation per guest operation.
    for (auto* monitor = monitors_; monitor; monitor = monitor->next_)
        try { monitor->retire(*this); } catch (...) { }
}
std::unique_ptr<execution::ExclusiveMonitor>
ExecutionMemory::create_exclusive_monitor()
{
    impl_->require_checked();
    return std::make_unique<Impl::Monitor>(impl_, *this);
}
}
