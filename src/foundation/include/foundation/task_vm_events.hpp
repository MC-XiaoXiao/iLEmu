// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
#pragma once

#include <atomic>
#include <cstdint>

namespace ilemu {

// XNU vm_fault charges the executing task, including faults against another
// task's map. Keep event history separate from AddressSpace and host residency.
class TaskVmEvents {
public:
    struct Snapshot {
        std::uint32_t faults { };
        std::uint32_t pageins { };
        std::uint32_t cow_faults { };
    };

    // Explicit execution/copy operations opt in. Inspection and compilation
    // use a null scope, including when nested inside a guest execution scope.
    // The owner must outlive the scope; concurrent guest lanes share its atomics.
    class Scope {
    public:
        explicit Scope(TaskVmEvents* actor) noexcept
            : previous_ { current_ }
        {
            current_ = actor;
        }
        ~Scope() { current_ = previous_; }
        Scope(const Scope&) = delete;
        Scope& operator=(const Scope&) = delete;
    private:
        TaskVmEvents* previous_;
    };

    [[nodiscard]] static TaskVmEvents* current() noexcept { return current_; }

    void fault() noexcept { faults_.fetch_add(1U, std::memory_order_relaxed); }
    void pagein() noexcept { pageins_.fetch_add(1U, std::memory_order_relaxed); }
    void copy_on_write() noexcept
    {
        cow_faults_.fetch_add(1U, std::memory_order_relaxed);
    }
    [[nodiscard]] Snapshot snapshot() const noexcept
    {
        // Native task_info does not promise a transaction across these fields.
        return { faults_.load(std::memory_order_relaxed),
            pageins_.load(std::memory_order_relaxed),
            cow_faults_.load(std::memory_order_relaxed) };
    }

private:
    inline static thread_local TaskVmEvents* current_ { };
    std::atomic<std::uint32_t> faults_ { };
    std::atomic<std::uint32_t> pageins_ { };
    std::atomic<std::uint32_t> cow_faults_ { };
};
} // namespace ilemu
