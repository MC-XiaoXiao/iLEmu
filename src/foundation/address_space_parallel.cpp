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
        auto lock = memory_.metadata_lock();
        if (!memory_.parallel_access_ || memory_.exclusive_write_observer_) {
            enabled_ = false;
            return;
        }
        if (!memory_.jit_read_page_table_ || !memory_.jit_write_page_table_) {
            // First allocation may refresh every global entry. Later channel
            // preparations only touch their own stable view.
            memory_.mutex_.quiesce();
            memory_.ensure_jit_page_tables_locked();
        }
        if (!memory_.parallel_state_ ||
            !memory_.parallel_state_->views.contains(slot)) {
            if (!memory_.parallel_state_)
                memory_.parallel_state_ = std::make_unique<ParallelState>();
            auto& entry = memory_.parallel_state_->views[slot];
            if (!entry)
                entry = std::make_unique<ParallelView>();
        }
        // Slot views have stable identities while other channels execute.
        // Never swap another channel's tables during thread migration.
        auto& destination = memory_.parallel_state_->views.at(slot);
        view_ = destination.get();
        memory_.mutex_.quiesce(
            [](const void* resource, const void* view) noexcept {
                return resource == view;
            }, view_);
        auto& entry = *view_;
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
        lease_.emplace(memory_.mutex_, interrupt, context,
            view_);
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
    auto& view = *scope->view_;
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
    auto& view = *scope->view_;
    return writing ? view.writes.entries() : view.reads.entries();
}

AddressSpace::WriteLock AddressSpace::scalar_access_lock(
    std::uint32_t address, std::size_t size, bool writing) const
{
    auto lock = metadata_lock();
    if (!lock.owns_lock())
        return lock;
    const auto* page = find_page_locked(address);
    // Only resident, private scalar data has a proven local mutation boundary.
    // Shared aliases, faults, COW and observers retain the full safe point.
    if (size == 0 || size > page_size ||
        (address & (page_size - 1U)) > page_size - size ||
        !page || !page->backing || page->shared_writable || page->file_cached ||
        page->copy_on_write_possible || exclusive_write_observer_ ||
        page->backing->shared_write_tracking_enabled()) {
        mutex_.quiesce();
        return lock;
    }
    struct Access {
        std::size_t index;
        bool writing;
    } access { address / page_size, writing };
    mutex_.quiesce([](const void* resource, const void* operation) noexcept {
        const auto& view = *static_cast<const ParallelView*>(resource);
        const auto& access = *static_cast<const Access*>(operation);
        return view.writes.entries()[access.index] ||
            (access.writing && view.reads.entries()[access.index]);
    }, &access);
    return lock;
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
    // Native lanes share the backend's coherent load/store domain. Do not
    // turn Guest sharing (including false sharing) into ownership ping-pong.
    // C++ checked accesses are a DIFFERENT domain: scalar_access_lock drains
    // every conflicting native view before reading or writing backing.bytes.
    // Existing guarded write entries still exclude reservations, COW, code
    // tracking and external/shared backing. ARM barriers remain in the JIT.
    auto& view = *scope->view_;
    view.touched.insert(base);
    view.reads.entries()[index] = jit_read_page_table_->entries()[index];
    if (writing && jit_write_page_table_)
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
