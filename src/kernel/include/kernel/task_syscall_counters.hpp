// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
#pragma once
#include <atomic>
#include <cstdint>

namespace ilemu {
// XNU counts kernel entries before validating the syscall. Aggregate directly
// at task lifetime: thread exit and ordinary exec do not discard this history.
// Relaxed counters let a remote task query without taking the caller's kernel
// mutex. The 32-bit ABI intentionally retains natural_t wraparound.
class TaskSyscallCounters {
public:
    struct Snapshot {
        std::uint32_t mach;
        std::uint32_t unix_calls;
    };
    void record_mach() { increment(mach_); }
    void record_unix() { increment(unix_); }
    [[nodiscard]] Snapshot snapshot() const
    {
        return { mach_.load(std::memory_order_relaxed),
            unix_.load(std::memory_order_relaxed) };
    }

private:
    // CompatibilityKernel serializes entries with its existing dispatch mutex.
    // Only readers are concurrent; avoid an unnecessary atomic read-modify-write.
    static void increment(std::atomic<std::uint32_t>& counter)
    {
        counter.store(counter.load(std::memory_order_relaxed) + 1U,
            std::memory_order_relaxed);
    }
    std::atomic<std::uint32_t> mach_ { };
    std::atomic<std::uint32_t> unix_ { };
};
} // namespace ilemu
