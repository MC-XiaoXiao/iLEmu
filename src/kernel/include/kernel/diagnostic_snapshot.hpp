/* SPDX-License-Identifier: MPL-2.0 */
#pragma once
#include "foundation/darwin_version.hpp"
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <span>
#include <string>
#include <vector>

namespace ilemu::diagnostic_snapshot {

struct Task {
    std::uint32_t pid;
    std::uint64_t unique_id;
    std::uint64_t flags;
    std::uint64_t start_wall_ns;
    std::uint64_t terminated_user_ns;
    std::uint64_t resident_bytes;
    std::uint64_t peak_resident_bytes;
    std::uint32_t suspend_count;
    std::string name;
};

struct Thread {
    std::uint64_t id;
    std::uint64_t user_ns;
    std::uint32_t state;
    std::int32_t base_priority;
    std::int32_t scheduled_priority;
    std::uint8_t requested_qos;
    bool suspended;
    std::vector<std::uint32_t> program_counters;
};

// XNU3789 kcdata.c append/alignment rules and kcdata.h record layouts.
class KcData {
public:
    explicit KcData(std::uint64_t now);
    void task(const Task& task);
    void thread(const Thread& thread);
    void end(std::uint64_t identifier);
    [[nodiscard]] std::vector<std::byte> finish();

private:
    void item(std::uint32_t type, std::span<const std::byte> payload,
        std::uint64_t flags = 0U);
    void container(std::uint32_t type, std::uint64_t identifier);
    std::vector<std::byte> bytes_;
};

struct TelemetryCapturePolicy {
    bool periodic_samples;
    static constexpr TelemetryCapturePolicy for_version(DarwinVersion version)
    {
        return { version >= DarwinGeneration::Ios6 };
    }
};

// Native microstackshot records retain PC-only samples, in chronological
// order, using XNU's 16 KiB ring and one-second global sampling period.
class Microstackshots {
public:
    static constexpr std::size_t capacity = 16U * 1024U;
    explicit Microstackshots(TelemetryCapturePolicy policy)
        : enabled_ { policy.periodic_samples } { }
    void enable(bool value) { enabled_.store(value, std::memory_order_relaxed); }
    [[nodiscard]] bool claim_sample(std::uint64_t now);
    void record(std::uint32_t processor, std::uint64_t wall_ns,
        const Task& task, const Thread& thread);
    [[nodiscard]] std::vector<std::byte> read() const;

private:
    using Record = std::array<std::byte, 640U>;
    std::atomic_bool enabled_;
    std::atomic_uint64_t next_sample_ { };
    mutable std::mutex mutex_;
    std::deque<Record> records_;
};

} // namespace ilemu::diagnostic_snapshot
