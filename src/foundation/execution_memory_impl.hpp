/* SPDX-License-Identifier: MPL-2.0 */
#pragma once
#include "foundation/execution_memory.hpp"
#include "foundation/address_space.hpp"
#include "foundation/arm_cpu_model.hpp"
#include <array>
#include <limits>
#include <map>
#include <stdexcept>

namespace ilemu {
inline constexpr std::uint32_t execution_page_base(std::uint32_t address)
{ return address & ~(AddressSpace::page_size - 1U); }
class ExecutionMemory::Impl {
public:
    class Monitor;
    ~Impl();
    Monitor* monitors_ = nullptr;
    struct PageStamp {
        std::uint64_t storage = 0, write = 0, shared_write = 0;
        bool operator==(const PageStamp&) const = default;
    };
    struct View {
        std::shared_ptr<const execution::CodeIdentity> identity =
            std::make_shared<execution::CodeIdentity>();
        std::uint64_t epoch = 1;
        bool initialized = false, has_second = false;
        PageStamp first_stamp, second_stamp;
        std::optional<std::uint64_t> observe(
            PageStamp first, std::optional<PageStamp> second = {}) noexcept
        {
            const bool changed = initialized &&
                (first != first_stamp || (second && has_second && *second != second_stamp));
            if (changed) {
                if (epoch == std::numeric_limits<std::uint64_t>::max())
                    return {};
                ++epoch;
                has_second = false;
            }
            initialized = true;
            first_stamp = first;
            if (second) {
                has_second = true;
                second_stamp = *second;
            }
            return epoch;
        }
    };
    class Lease final : public execution::InstructionLease {
        struct Page {
            std::uint32_t base = 0;
            std::shared_ptr<GuestPageBacking> backing;
            std::span<const std::byte> bytes;
            std::uint64_t write_generation = 0;
            std::shared_lock<std::shared_mutex> lock;
            std::uint64_t generation() const noexcept
            {
                return backing ? backing->shared_write_generation() : 0;
            }
            PageStamp stamp() const noexcept
            {
                return { backing ? backing->storage_identity() : 0,
                    write_generation, generation() };
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
        std::optional<execution::InstructionRegion>
        continuation_region() const noexcept override
        {
            if (count_ == 0 || !pages_[0].backing ||
                pages_[0].bytes.size() != AddressSpace::page_size)
                return {};
            const auto begin = std::uint64_t { pages_[0].base };
            return execution::InstructionRegion {
                begin, begin + AddressSpace::page_size };
        }
        std::optional<execution::CompilationIdentity>
        compilation_identity() const noexcept override
        {
            auto semantics = owner_.model_.compilation_identity();
            if (count_ == 0 || !semantics)
                return {};
            execution::CompilationIdentity result { std::move(semantics), {}, count_ };
            for (unsigned i = 0; i < count_; ++i) {
                const auto stamp = pages_[i].stamp();
                if (stamp.storage == 0)
                    return {};
                result.pages[i] = { stamp.storage, stamp.write, stamp.shared_write };
            }
            return result;
        }
        std::optional<std::uint64_t> generation() const noexcept override
        {
            if (count_ == 0)
                return std::nullopt;
            return view_.observe(pages_[0].stamp(),
                count_ == 2 ? std::optional { pages_[1].stamp() } : std::nullopt);
        }
        std::optional<execution::LeasedCodeView>
        cached_continuation(std::uint32_t pc) noexcept override
        {
            const auto base = execution_page_base(pc);
            const auto begin = std::uint64_t { base };
            if (std::uint64_t { pc } + 4 > begin + AddressSpace::page_size)
                return {};
            if (auto region = continuation_region(); region && base == pages_[0].base)
                return execution::LeasedCodeView { identity(), generation(), *region };
            // Do not allocate or erase view metadata while the primary View&
            // is borrowed. An unobserved page cannot have a reusable entry.
            const auto found = owner_.views_.find(base);
            if (found == owner_.views_.end())
                return {};
            try {
                Page* page = nullptr;
                if (continued_)
                    for (unsigned i = 0; i < continued_->count; ++i)
                        if (continued_->pages[i].base == base)
                            page = &continued_->pages[i];
                if (!page) {
                    if (continued_ && continued_->count == continued_->pages.size())
                        return {};
                    auto ready = owner_.memory_.instruction_page_view(pc);
                    if (!ready || !ready->backing || ready->bytes.size() != AddressSpace::page_size)
                        return {};
                    Page candidate;
                    candidate.base = base;
                    candidate.backing = std::move(ready->backing);
                    candidate.bytes = ready->bytes;
                    candidate.write_generation = ready->write_generation;
                    bool already_locked = false;
                    for (unsigned i = 0; i < count_; ++i)
                        already_locked |= pages_[i].backing == candidate.backing;
                    if (continued_)
                        for (unsigned i = 0; i < continued_->count; ++i)
                            already_locked |= continued_->pages[i].backing == candidate.backing;
                    if (!already_locked) {
                        candidate.lock = candidate.backing->try_lock_instruction_read();
                        if (!candidate.lock.owns_lock())
                            return {};
                    }
                    if (!continued_)
                        continued_ = std::make_unique<ContinuedPages>();
                    page = &continued_->pages[continued_->count++];
                    *page = std::move(candidate);
                }
                auto& view = found->second;
                return execution::LeasedCodeView { view.identity,
                    view.observe(page->stamp()), { begin, begin + AddressSpace::page_size } };
            } catch (...) {
                // Optional pinning cannot throw through a native continuation.
                return {};
            }
        }
        template <typename T>
        std::optional<T> fetch(std::uint32_t address) const noexcept
        {
            if (std::uint64_t { address } + sizeof(T) >
                (std::uint64_t { 1 } << 32U))
                return { };
            const auto* page = find_page(address);
            if (!page)
                return { };
            const auto offset = address - page->base;
            if (offset <= AddressSpace::page_size - sizeof(T))
                return static_cast<T>(read(*page, offset, sizeof(T)));
            std::uint32_t value = 0;
            for (unsigned byte = 0; byte < sizeof(T); ++byte) {
                const auto current = address + byte;
                page = find_page(current);
                if (!page)
                    return { };
                value |= read(*page, current - page->base, 1) << (byte * 8U);
            }
            return static_cast<T>(value);
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
            const auto base = execution_page_base(address);
            for (unsigned i = 0; i < count_; ++i)
                if (pages_[i].base == base)
                    return { };
            if (count_ == pages_.size())
                throw std::logic_error("instruction lease exceeds two pages");
            auto view = owner_.memory_.instruction_page_view(address);
            if (!view)
                return owner_.fault(address, size, MemoryPermission::Execute);
            auto& page = pages_[count_];
            page.base = base;
            page.backing = std::move(view->backing);
            page.bytes = view->bytes;
            page.write_generation = view->write_generation;
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
        const Page* find_page(std::uint32_t address) const noexcept
        {
            const auto base = execution_page_base(address);
            for (unsigned i = 0; i < count_; ++i)
                if (pages_[i].base == base)
                    return &pages_[i];
            return nullptr;
        }
        static std::uint32_t read(
            const Page& page, unsigned offset, unsigned size) noexcept
        {
            std::uint32_t value = 0;
            if (!page.bytes.empty())
                for (unsigned byte = 0; byte < size; ++byte)
                    value |= std::to_integer<std::uint32_t>(
                                 page.bytes[offset + byte]) << (byte * 8U);
            return value;
        }
        Impl& owner_;
        AddressSpace::ExclusiveAccess memory_;
        View& view_;
        std::uint32_t pc_;
        bool thumb_;
        std::array<Page, 2> pages_;
        unsigned count_ = 0;
        // Bounded invocation-only pins; never retained in the translation cache.
        struct ContinuedPages {
            std::array<Page, 4> pages;
            unsigned count = 0;
        };
        std::unique_ptr<ContinuedPages> continued_;
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
}
