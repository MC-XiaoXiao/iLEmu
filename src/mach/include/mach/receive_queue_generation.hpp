// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <bitset>
#include <cstddef>
#include <cstdint>

namespace ilemu::xnu::ipc {

// Conservative empty-receive invalidation without a per-poll object lookup.
// Writers hold the IPC lock; readers can check snapshots without that lock.
// Hash collisions only cause extra polling. Rights and topology changes
// invalidate every bucket so a cached empty receive cannot hide invalid rights.
class ReceiveQueueGeneration {
public:
    static constexpr std::size_t bucket_count = 1024;
    using DependencyMask = std::bitset<bucket_count>;
    void note_enqueue(std::uint32_t object)
    {
        const auto next = global_.load(std::memory_order_relaxed) + 1U;
        const auto changed_bucket = bucket(object);
        // One atomic record includes sequence identity and bucket. A reader
        // racing ring overwrite rejects the record rather than missing a wake.
        changes_[next % journal_size].store(
            (next << bucket_bits) | changed_bucket, std::memory_order_release);
        objects_[changed_bucket].store(next, std::memory_order_release);
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

    static void add_dependency(DependencyMask& mask, std::uint32_t object)
    {
        mask.set(bucket(object));
    }

    // A bounded journal lets a group of negative observations reject unrelated
    // IPC without walking every knote. Overflow, topology changes and a racing
    // overwrite all conservatively request the existing per-object probe.
    [[nodiscard]] bool changed_since(std::uint64_t observed,
        const DependencyMask& dependencies, std::size_t maximum_changes) const
    {
        const auto current = snapshot();
        if (current == observed)
            return false;
        if (current < observed ||
            topology_.load(std::memory_order_acquire) > observed)
            return true;
        if (dependencies.none())
            return false;
        const auto distance = current - observed;
        if (distance > std::min(journal_size, maximum_changes))
            return true;
        constexpr auto bucket_mask = bucket_count - 1U;
        for (std::uint64_t offset = 1; offset <= distance; ++offset) {
            const auto sequence = observed + offset;
            const auto record = changes_[sequence % journal_size].load(
                std::memory_order_acquire);
            if ((record & ~std::uint64_t { bucket_mask }) !=
                    (sequence << bucket_bits) ||
                dependencies.test(record & bucket_mask))
                return true;
        }
        return false;
    }

private:
    static constexpr unsigned bucket_bits = 10;
    static constexpr std::size_t journal_size = 256;

    [[nodiscard]] static constexpr std::size_t bucket(std::uint32_t object)
    {
        // Object identifiers normally advance by 0x100. Fold the upper bits
        // as well as the low byte to avoid concentrating aligned identifiers.
        return (object ^ (object >> 8U) ^ (object >> 18U)) & (bucket_count - 1U);
    }

    std::atomic_uint64_t global_ { 1 };
    std::atomic_uint64_t topology_ { 1 };
    std::array<std::atomic_uint64_t, bucket_count> objects_ { };
    std::array<std::atomic_uint64_t, journal_size> changes_ { };
};

} // namespace ilemu::xnu::ipc
