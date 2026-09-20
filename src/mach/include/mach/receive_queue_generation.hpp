// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

namespace ilemu::xnu::ipc {

// Conservative empty-receive invalidation without a per-poll object lookup.
// Writers hold the IPC lock; readers can check snapshots without that lock.
// Hash collisions only cause extra polling. Rights and topology changes
// invalidate every bucket so a cached empty receive cannot hide invalid rights.
class ReceiveQueueGeneration {
public:
    void note_enqueue(std::uint32_t object)
    {
        const auto next = global_.load(std::memory_order_relaxed) + 1U;
        objects_[bucket(object)].store(next, std::memory_order_release);
        // Publish the scoped change before waking outer scheduler caches.
        global_.store(next, std::memory_order_release);
    }

    void note_topology_change()
    {
        const auto next = global_.load(std::memory_order_relaxed) + 1U;
        topology_.store(next, std::memory_order_release);
        global_.store(next, std::memory_order_release);
    }

    [[nodiscard]] std::uint64_t snapshot() const
    {
        return global_.load(std::memory_order_acquire);
    }

    [[nodiscard]] std::uint64_t snapshot(std::uint32_t object) const
    {
        return std::max(topology_.load(std::memory_order_acquire),
            objects_[bucket(object)].load(std::memory_order_acquire));
    }

private:
    static constexpr std::size_t bucket_count = 1024;

    [[nodiscard]] static constexpr std::size_t bucket(std::uint32_t object)
    {
        // Object identifiers normally advance by 0x100. Fold the upper bits
        // as well as the low byte to avoid concentrating aligned identifiers.
        return (object ^ (object >> 8U) ^ (object >> 18U)) & (bucket_count - 1U);
    }

    std::atomic_uint64_t global_ { 1 };
    std::atomic_uint64_t topology_ { 1 };
    std::array<std::atomic_uint64_t, bucket_count> objects_ { };
};

} // namespace ilemu::xnu::ipc
