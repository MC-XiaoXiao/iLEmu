#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace ilemu {

enum class PerfFallbackReason : std::uint8_t {
    None,
    VulkanUnavailable,
    InvalidTarget,
    UnsupportedPrimitive,
    InvalidVertex,
    UnsupportedBlend,
    PipelineUnavailable,
    TargetBusy,
    BackendFailure,
    Count,
};

inline constexpr auto perf_fallback_reason_count =
    static_cast<std::size_t>(PerfFallbackReason::Count);

struct PerformanceSnapshot {
    std::uint64_t jit_instances{};
    std::uint64_t translation_blocks{};
    std::uint64_t cpu_executions{};
    std::uint64_t cpu_ticks{};
    std::uint64_t svc_calls{};
    std::uint64_t page_misses{};
    std::uint64_t page_faults{};
    std::uint64_t draws{};
    std::uint64_t submits{};
    std::uint64_t fence_waits{};
    std::uint64_t fence_wait_nanoseconds{};
    std::uint64_t upload_bytes{};
    std::uint64_t readback_bytes{};
    std::array<std::uint64_t, perf_fallback_reason_count> fallback_reasons{};
};

class PerformanceCounters {
  public:
    void reset(bool enabled);
    [[nodiscard]] bool enabled() const {
        return enabled_.load(std::memory_order_relaxed);
    }

    void record_jit();
    void record_translation_block();
    void record_cpu_execution(std::uint64_t ticks);
    void record_svc();
    void record_page_miss();
    void record_page_fault();
    void record_draw();
    void record_submit();
    void record_fence_wait(std::uint64_t nanoseconds);
    void record_upload(std::uint64_t bytes);
    void record_readback(std::uint64_t bytes);
    void record_fallback(PerfFallbackReason reason);

    [[nodiscard]] PerformanceSnapshot snapshot() const;

  private:
    std::atomic<bool> enabled_{false};
    std::atomic<std::uint64_t> jit_instances_{};
    std::atomic<std::uint64_t> translation_blocks_{};
    std::atomic<std::uint64_t> cpu_executions_{};
    std::atomic<std::uint64_t> cpu_ticks_{};
    std::atomic<std::uint64_t> svc_calls_{};
    std::atomic<std::uint64_t> page_misses_{};
    std::atomic<std::uint64_t> page_faults_{};
    std::atomic<std::uint64_t> draws_{};
    std::atomic<std::uint64_t> submits_{};
    std::atomic<std::uint64_t> fence_waits_{};
    std::atomic<std::uint64_t> fence_wait_nanoseconds_{};
    std::atomic<std::uint64_t> upload_bytes_{};
    std::atomic<std::uint64_t> readback_bytes_{};
    std::array<std::atomic<std::uint64_t>, perf_fallback_reason_count>
        fallback_reasons_{};
};

[[nodiscard]] PerformanceCounters& performance_counters();
[[nodiscard]] std::string_view
perf_fallback_reason_name(PerfFallbackReason reason);
[[nodiscard]] std::string
format_performance_summary(const PerformanceSnapshot& snapshot);

} // namespace ilemu
