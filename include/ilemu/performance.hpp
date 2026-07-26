#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

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

enum class PerfCpuMapReason : std::uint8_t {
    Internal,
    HostUpload,
    GpuReadback,
    CoreSurface,
    SoftwareFallback,
    DeferredDisplayRead,
    NativePresent,
    Count,
};

inline constexpr auto perf_cpu_map_reason_count =
    static_cast<std::size_t>(PerfCpuMapReason::Count);

enum class PerfSurfaceKind : std::uint8_t {
    Unknown,
    CoreSurface,
    Scanout,
    GlesRenderTarget,
    Count,
};

inline constexpr auto perf_surface_kind_count =
    static_cast<std::size_t>(PerfSurfaceKind::Count);

struct HlePerformanceSnapshot {
    std::string subsystem;
    std::uint64_t calls{};
    std::uint64_t nanoseconds{};
};

struct PerformanceSnapshot {
    std::uint64_t jit_instances{};
    std::uint64_t jit_live_instances{};
    std::uint64_t jit_live_peak_instances{};
    std::uint64_t jit_creation_nanoseconds{};
    std::uint64_t jit_code_cache_bytes{};
    std::uint64_t jit_code_cache_peak_bytes{};
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
    std::uint64_t host_fills{};
    std::uint64_t host_copies{};
    std::uint64_t native_presents{};
    std::uint64_t cpu_present_fallbacks{};
    std::uint64_t cpu_map_reads{};
    std::uint64_t cpu_map_writes{};
    std::array<std::uint64_t, perf_cpu_map_reason_count>
        cpu_map_read_reasons{};
    std::array<std::uint64_t, perf_cpu_map_reason_count>
        cpu_map_write_reasons{};
    std::array<std::uint64_t, perf_surface_kind_count>
        surface_upload_bytes{};
    std::array<std::uint64_t, perf_surface_kind_count>
        surface_readback_bytes{};
    std::uint64_t forks{};
    std::uint64_t execs{};
    std::uint64_t abnormal_exits{};
    std::array<std::uint64_t, perf_fallback_reason_count> fallback_reasons{};
    std::vector<HlePerformanceSnapshot> hle_subsystems;
};

class PerformanceCounters {
  public:
    void reset(bool enabled);
    [[nodiscard]] bool enabled() const {
        return enabled_.load(std::memory_order_relaxed);
    }

    void record_jit(std::uint64_t creation_nanoseconds = 0);
    void record_jit_destroyed();
    void record_jit_code_cache_usage(std::uint64_t previous_bytes,
                                     std::uint64_t current_bytes);
    void record_translation_block();
    void record_cpu_execution(std::uint64_t ticks);
    void record_svc();
    void record_page_miss();
    void record_page_fault();
    void record_draw();
    void record_submit();
    void record_fence_wait(std::uint64_t nanoseconds);
    void record_upload(std::uint64_t bytes,
                       PerfSurfaceKind surface = PerfSurfaceKind::Unknown);
    void record_readback(std::uint64_t bytes,
                         PerfSurfaceKind surface = PerfSurfaceKind::Unknown);
    void record_host_fill();
    void record_host_copy();
    void record_native_present();
    void record_cpu_present_fallback();
    void record_cpu_map(bool write, PerfCpuMapReason reason);
    void record_hle(std::string_view subsystem, std::uint64_t nanoseconds);
    void record_fork();
    void record_exec();
    void record_abnormal_exit();
    void record_fallback(PerfFallbackReason reason);

    [[nodiscard]] PerformanceSnapshot snapshot() const;

  private:
    std::atomic<bool> enabled_{false};
    std::atomic<std::uint64_t> jit_instances_{};
    std::atomic<std::uint64_t> jit_live_instances_{};
    std::atomic<std::uint64_t> jit_live_peak_instances_{};
    std::atomic<std::uint64_t> jit_creation_nanoseconds_{};
    std::atomic<std::uint64_t> jit_code_cache_current_bytes_{};
    std::atomic<std::uint64_t> jit_code_cache_peak_bytes_{};
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
    std::atomic<std::uint64_t> host_fills_{};
    std::atomic<std::uint64_t> host_copies_{};
    std::atomic<std::uint64_t> native_presents_{};
    std::atomic<std::uint64_t> cpu_present_fallbacks_{};
    std::atomic<std::uint64_t> cpu_map_reads_{};
    std::atomic<std::uint64_t> cpu_map_writes_{};
    std::array<std::atomic<std::uint64_t>, perf_cpu_map_reason_count>
        cpu_map_read_reasons_{};
    std::array<std::atomic<std::uint64_t>, perf_cpu_map_reason_count>
        cpu_map_write_reasons_{};
    std::array<std::atomic<std::uint64_t>, perf_surface_kind_count>
        surface_upload_bytes_{};
    std::array<std::atomic<std::uint64_t>, perf_surface_kind_count>
        surface_readback_bytes_{};
    std::atomic<std::uint64_t> forks_{};
    std::atomic<std::uint64_t> execs_{};
    std::atomic<std::uint64_t> abnormal_exits_{};
    std::array<std::atomic<std::uint64_t>, perf_fallback_reason_count>
        fallback_reasons_{};
    mutable std::mutex hle_mutex_;
    std::map<std::string, HlePerformanceSnapshot, std::less<>>
        hle_subsystems_;
};

[[nodiscard]] PerformanceCounters& performance_counters();
[[nodiscard]] std::string_view
perf_fallback_reason_name(PerfFallbackReason reason);
[[nodiscard]] std::string_view
perf_cpu_map_reason_name(PerfCpuMapReason reason);
[[nodiscard]] std::string_view
perf_surface_kind_name(PerfSurfaceKind kind);
[[nodiscard]] std::string
format_performance_summary(const PerformanceSnapshot& snapshot);

} // namespace ilemu
