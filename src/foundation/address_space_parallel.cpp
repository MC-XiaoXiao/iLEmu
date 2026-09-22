// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
#include "foundation/address_space.hpp"

namespace ilemu {

thread_local AddressSpace::ParallelAccess* AddressSpace::parallel_scope_ { };

AddressSpace::ParallelAccess* AddressSpace::parallel_scope() const noexcept
{
    for (auto* scope = parallel_scope_; scope; scope = scope->previous_)
        if (&scope->memory_ == this)
            return scope;
    return nullptr;
}

AddressSpace::ParallelAccess::ParallelAccess(
    AddressSpace& memory, std::size_t slot, std::size_t owner)
    : memory_ { memory }, slot_ { slot }, owner_ { owner }, previous_ { parallel_scope_ }
{
    parallel_scope_ = this;
    if (memory_.owns_exclusive_access()) {
        enabled_ = false;
        return;
    }
    try {
        auto lock = memory_.write_lock();
        if (!memory_.parallel_access_ || memory_.exclusive_write_observer_) {
            enabled_ = false;
            return;
        }
        if (!memory_.parallel_state_ ||
            !memory_.parallel_state_->views.contains(slot)) {
            if (!memory_.parallel_state_)
                memory_.parallel_state_ = std::make_unique<ParallelState>();
            auto& entry = memory_.parallel_state_->views[slot];
            if (!entry)
                entry = std::make_unique<ParallelView>();
        }
        // Prepare before the batch entry rendezvous; no read lease may
        // block another lane that is still binding its execution state.
        auto& views = memory_.parallel_state_->views;
        auto& destination = views.at(slot);
        if (destination->owner != owner) {
            // Scheduler migration changes the executor slot, not page
            // ownership. Move the existing view during batch preparation,
            // before any lane can hold a native lease or use its links.
            for (auto& [other_slot, candidate] : views) {
                if (other_slot != slot && candidate->owner == owner) {
                    destination.swap(candidate);
                    break;
                }
            }
        }
        auto& entry = *destination;
        if (entry.owner != owner) {
            for (auto page : entry.touched) {
                entry.reads.entries()[page / page_size] = nullptr;
                entry.writes.entries()[page / page_size] = nullptr;
            }
            entry.touched.clear();
            entry.owner = owner;
        }
    } catch (...) {
        lease_.reset();
        parallel_scope_ = previous_;
        throw;
    }
}

void AddressSpace::ParallelAccess::activate(
    GuestMemoryGate::Lease::Interrupt interrupt, void* context)
{
    if (enabled_)
        lease_.emplace(memory_.mutex_, interrupt, context);
}

AddressSpace::ParallelAccess::~ParallelAccess()
{
    lease_.reset();
    parallel_scope_ = previous_;
}

void AddressSpace::suspend_parallel_access()
{
    if (auto* scope = parallel_scope(); scope && scope->enabled_ && scope->lease_)
        scope->lease_->suspend();
}

void AddressSpace::resume_parallel_access()
{
    if (auto* scope = parallel_scope(); scope && scope->enabled_ && scope->lease_)
        scope->lease_->resume();
}

void AddressSpace::leave_parallel_access()
{
    auto* scope = parallel_scope();
    if (!scope || !scope->enabled_)
        return;
    auto lock = write_lock();
    auto& view = *parallel_state_->views.at(scope->slot_);
    for (auto page : view.touched) {
        view.reads.entries()[page / page_size] = nullptr;
        view.writes.entries()[page / page_size] = nullptr;
    }
    view.touched.clear();
    scope->lease_->disable();
    scope->enabled_ = false;
}

std::uint8_t** AddressSpace::parallel_table_locked(bool writing)
{
    const auto* scope = parallel_scope();
    if (!scope || !scope->enabled_)
        return nullptr;
    auto& view = *parallel_state_->views.at(scope->slot_);
    return writing ? view.writes.entries() : view.reads.entries();
}

void AddressSpace::grant_parallel_page_locked(std::uint32_t address, bool writing)
{
    const auto* scope = parallel_scope();
    if (!scope || !scope->enabled_ || !jit_page_table_enabled_ ||
        exclusive_write_observer_)
        return;
    const auto base = address & ~(page_size - 1U);
    const auto index = base / page_size;
    const auto* page = find_page_locked(base);
    // Cross-address-space writable aliases and observed shared storage retain
    // the existing checked path. COW writes detach before reaching this point.
    if (!page || !page->backing || page->shared_writable ||
        page->backing->shared_write_tracking_enabled() ||
        !jit_read_page_table_ || !jit_read_page_table_->entries()[index])
        return;
    auto& state = parallel_state_->pages.try_emplace(
        base, ParallelPage { scope->owner_ }).first.value();
    const bool previously_shared_writable = state.shared && state.written;
    state.shared = state.shared || state.first_reader != scope->owner_;
    state.written = state.written || writing;
    if (state.shared && state.written && !previously_shared_writable) {
        // Drain native readers before revoking the sole writer. Shared reads
        // remain direct: a checked store requests a block-boundary exit from
        // readers and acquires the exclusive gate before changing any bytes.
        for (auto& [slot, entry] : parallel_state_->views) {
            static_cast<void>(slot);
            if (entry->touched.contains(base))
                entry->writes.entries()[index] = nullptr;
        }
    }
    auto& view = *parallel_state_->views.at(scope->slot_);
    view.touched.insert(base);
    view.reads.entries()[index] = jit_read_page_table_->entries()[index];
    if (writing && !state.shared && jit_write_page_table_)
        view.writes.entries()[index] = jit_write_page_table_->entries()[index];
}

void AddressSpace::invalidate_parallel_page_locked(std::uint32_t address)
{
    if (!parallel_state_)
        return;
    const auto base = address & ~(page_size - 1U);
    for (auto& [slot, entry] : parallel_state_->views) {
        static_cast<void>(slot);
        if (entry->touched.erase(base)) {
            entry->reads.entries()[base / page_size] = nullptr;
            entry->writes.entries()[base / page_size] = nullptr;
        }
    }
    // Keep sharing history across protection/guard refreshes. A remapped page
    // may remain conservatively checked, but must never regain a stale writer.
}

void AddressSpace::clear_parallel_views_locked()
{
    if (!parallel_state_)
        return;
    for (auto& [slot, entry] : parallel_state_->views) {
        static_cast<void>(slot);
        for (auto page : entry->touched) {
            entry->reads.entries()[page / page_size] = nullptr;
            entry->writes.entries()[page / page_size] = nullptr;
        }
        entry->touched.clear();
    }
}

} // namespace ilemu
