// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <unordered_map>
#include <utility>

namespace ilemu {

// Global IPC object identity, independent of task-local Mach names. Callers
// hold KernelSharedState::mach_mutex for both reads and mutations.
class MachThreadObjectRegistry {
public:
    using Owner = std::pair<std::uint32_t, std::uint32_t>;
    using Slots = std::map<std::uint32_t, std::uint32_t>;
    using Tasks = std::map<std::uint32_t, Slots>;

    // Enumeration remains ordered and read-only: mutations must update both
    // directions, including slot reuse, exec and object/task destruction.
    [[nodiscard]] auto begin() const { return tasks_.begin(); }
    [[nodiscard]] auto end() const { return tasks_.end(); }
    [[nodiscard]] auto find(std::uint32_t pid) const { return tasks_.find(pid); }

    [[nodiscard]] std::optional<Owner> owner(std::uint32_t object) const
    {
        const auto found = owners_.find(object);
        if (found == owners_.end())
            return std::nullopt;
        return *found->second.begin();
    }

    void set(std::uint32_t pid, std::uint32_t slot, std::uint32_t object)
    {
        std::optional<std::uint32_t> previous;
        if (const auto task = tasks_.find(pid); task != tasks_.end()) {
            if (const auto thread = task->second.find(slot);
                thread != task->second.end()) {
                if (thread->second == object)
                    return;
                previous = thread->second;
            }
        }
        const Owner owner { pid, slot };
        const auto entry = owners_.try_emplace(object).first;
        try {
            entry->second.insert(owner);
            auto& slots = tasks_[pid];
            try {
                slots.insert_or_assign(slot, object);
            } catch (...) {
                if (slots.empty())
                    tasks_.erase(pid);
                throw;
            }
        } catch (...) {
            erase_association(object, owner);
            throw;
        }
        if (previous)
            erase_association(*previous, owner);
    }

    void erase_slot(std::uint32_t pid, std::uint32_t slot)
    {
        const auto task = tasks_.find(pid);
        if (task == tasks_.end())
            return;
        const auto thread = task->second.find(slot);
        if (thread == task->second.end())
            return;
        erase_association(thread->second, { pid, slot });
        task->second.erase(thread);
        if (task->second.empty())
            tasks_.erase(task);
    }

    void erase_task(std::uint32_t pid)
    {
        const auto task = tasks_.find(pid);
        if (task == tasks_.end())
            return;
        for (const auto& [slot, object] : task->second)
            erase_association(object, { pid, slot });
        tasks_.erase(task);
    }

    void erase_object(std::uint32_t object)
    {
        const auto detached = owners_.extract(object);
        if (detached.empty())
            return;
        for (const auto& [pid, slot] : detached.mapped()) {
            const auto task = tasks_.find(pid);
            task->second.erase(slot);
            if (task->second.empty())
                tasks_.erase(task);
        }
    }

private:
    void erase_association(std::uint32_t object, Owner owner)
    {
        const auto found = owners_.find(object);
        if (found == owners_.end())
            return;
        found->second.erase(owner);
        if (found->second.empty())
            owners_.erase(found);
    }

    Tasks tasks_;
    // An object normally names one slot. Ordered aliases preserve the former
    // PID/slot traversal's first match without scanning unrelated tasks.
    std::unordered_map<std::uint32_t, std::set<Owner>> owners_;
};

} // namespace ilemu
