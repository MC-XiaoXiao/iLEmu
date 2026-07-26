#include "ilemu/performance.hpp"

#include <sstream>

namespace ilemu {
namespace {

void add_if_enabled(std::atomic<std::uint64_t>& counter,
                    const std::atomic<bool>& enabled,
                    std::uint64_t value = 1) {
    if (enabled.load(std::memory_order_relaxed)) {
        counter.fetch_add(value, std::memory_order_relaxed);
    }
}

} // namespace

void PerformanceCounters::reset(bool enabled) {
    enabled_.store(false, std::memory_order_relaxed);
    jit_instances_.store(0, std::memory_order_relaxed);
    jit_live_instances_.store(0, std::memory_order_relaxed);
    jit_live_peak_instances_.store(0, std::memory_order_relaxed);
    jit_creation_nanoseconds_.store(0, std::memory_order_relaxed);
    jit_code_cache_current_bytes_.store(0, std::memory_order_relaxed);
    jit_code_cache_peak_bytes_.store(0, std::memory_order_relaxed);
    translation_blocks_.store(0, std::memory_order_relaxed);
    cpu_executions_.store(0, std::memory_order_relaxed);
    cpu_ticks_.store(0, std::memory_order_relaxed);
    svc_calls_.store(0, std::memory_order_relaxed);
    page_misses_.store(0, std::memory_order_relaxed);
    page_faults_.store(0, std::memory_order_relaxed);
    draws_.store(0, std::memory_order_relaxed);
    submits_.store(0, std::memory_order_relaxed);
    fence_waits_.store(0, std::memory_order_relaxed);
    fence_wait_nanoseconds_.store(0, std::memory_order_relaxed);
    upload_bytes_.store(0, std::memory_order_relaxed);
    readback_bytes_.store(0, std::memory_order_relaxed);
    host_fills_.store(0, std::memory_order_relaxed);
    host_copies_.store(0, std::memory_order_relaxed);
    native_presents_.store(0, std::memory_order_relaxed);
    cpu_present_fallbacks_.store(0, std::memory_order_relaxed);
    cpu_map_reads_.store(0, std::memory_order_relaxed);
    cpu_map_writes_.store(0, std::memory_order_relaxed);
    forks_.store(0, std::memory_order_relaxed);
    execs_.store(0, std::memory_order_relaxed);
    abnormal_exits_.store(0, std::memory_order_relaxed);
    for (auto& counter : cpu_map_read_reasons_)
        counter.store(0, std::memory_order_relaxed);
    for (auto& counter : cpu_map_write_reasons_)
        counter.store(0, std::memory_order_relaxed);
    for (auto& counter : surface_upload_bytes_)
        counter.store(0, std::memory_order_relaxed);
    for (auto& counter : surface_readback_bytes_)
        counter.store(0, std::memory_order_relaxed);
    for (auto& counter : fallback_reasons_)
        counter.store(0, std::memory_order_relaxed);
    {
        std::lock_guard lock{hle_mutex_};
        hle_subsystems_.clear();
    }
    enabled_.store(enabled, std::memory_order_release);
}

void PerformanceCounters::record_jit(std::uint64_t creation_nanoseconds) {
    if (!enabled())
        return;
    jit_instances_.fetch_add(1, std::memory_order_relaxed);
    jit_creation_nanoseconds_.fetch_add(
        creation_nanoseconds, std::memory_order_relaxed);
    const auto live =
        jit_live_instances_.fetch_add(1, std::memory_order_relaxed) + 1;
    auto peak = jit_live_peak_instances_.load(std::memory_order_relaxed);
    while (live > peak &&
           !jit_live_peak_instances_.compare_exchange_weak(
               peak, live, std::memory_order_relaxed,
               std::memory_order_relaxed)) {
    }
}

void PerformanceCounters::record_jit_destroyed() {
    if (!enabled())
        return;
    auto live = jit_live_instances_.load(std::memory_order_relaxed);
    while (live != 0 &&
           !jit_live_instances_.compare_exchange_weak(
               live, live - 1, std::memory_order_relaxed,
               std::memory_order_relaxed)) {
    }
}

void PerformanceCounters::record_jit_code_cache_usage(
    std::uint64_t previous_bytes, std::uint64_t current_bytes) {
    if (!enabled() || previous_bytes == current_bytes)
        return;
    std::uint64_t aggregate{};
    if (current_bytes > previous_bytes) {
        aggregate = jit_code_cache_current_bytes_.fetch_add(
                        current_bytes - previous_bytes,
                        std::memory_order_relaxed) +
                    current_bytes - previous_bytes;
    } else {
        aggregate = jit_code_cache_current_bytes_.fetch_sub(
                        previous_bytes - current_bytes,
                        std::memory_order_relaxed) -
                    (previous_bytes - current_bytes);
    }
    auto peak =
        jit_code_cache_peak_bytes_.load(std::memory_order_relaxed);
    while (aggregate > peak &&
           !jit_code_cache_peak_bytes_.compare_exchange_weak(
               peak, aggregate, std::memory_order_relaxed,
               std::memory_order_relaxed)) {
    }
}

void PerformanceCounters::record_translation_block() {
    add_if_enabled(translation_blocks_, enabled_);
}

void PerformanceCounters::record_cpu_execution(std::uint64_t ticks) {
    if (!enabled())
        return;
    cpu_executions_.fetch_add(1, std::memory_order_relaxed);
    cpu_ticks_.fetch_add(ticks, std::memory_order_relaxed);
}

void PerformanceCounters::record_svc() {
    add_if_enabled(svc_calls_, enabled_);
}

void PerformanceCounters::record_page_miss() {
    add_if_enabled(page_misses_, enabled_);
}

void PerformanceCounters::record_page_fault() {
    add_if_enabled(page_faults_, enabled_);
}

void PerformanceCounters::record_draw() {
    add_if_enabled(draws_, enabled_);
}

void PerformanceCounters::record_submit() {
    add_if_enabled(submits_, enabled_);
}

void PerformanceCounters::record_fence_wait(std::uint64_t nanoseconds) {
    if (!enabled())
        return;
    fence_waits_.fetch_add(1, std::memory_order_relaxed);
    fence_wait_nanoseconds_.fetch_add(nanoseconds, std::memory_order_relaxed);
}

void PerformanceCounters::record_upload(std::uint64_t bytes,
                                        PerfSurfaceKind surface) {
    add_if_enabled(upload_bytes_, enabled_, bytes);
    const auto index = static_cast<std::size_t>(surface);
    if (enabled() && index < surface_upload_bytes_.size()) {
        surface_upload_bytes_[index].fetch_add(bytes,
                                               std::memory_order_relaxed);
    }
}

void PerformanceCounters::record_readback(std::uint64_t bytes,
                                          PerfSurfaceKind surface) {
    add_if_enabled(readback_bytes_, enabled_, bytes);
    const auto index = static_cast<std::size_t>(surface);
    if (enabled() && index < surface_readback_bytes_.size()) {
        surface_readback_bytes_[index].fetch_add(
            bytes, std::memory_order_relaxed);
    }
}

void PerformanceCounters::record_host_fill() {
    add_if_enabled(host_fills_, enabled_);
}

void PerformanceCounters::record_host_copy() {
    add_if_enabled(host_copies_, enabled_);
}

void PerformanceCounters::record_native_present() {
    add_if_enabled(native_presents_, enabled_);
}

void PerformanceCounters::record_cpu_present_fallback() {
    add_if_enabled(cpu_present_fallbacks_, enabled_);
}

void PerformanceCounters::record_cpu_map(bool write,
                                         PerfCpuMapReason reason) {
    const auto index = static_cast<std::size_t>(reason);
    if (!enabled() || index >= perf_cpu_map_reason_count)
        return;
    auto& total = write ? cpu_map_writes_ : cpu_map_reads_;
    auto& reasons =
        write ? cpu_map_write_reasons_ : cpu_map_read_reasons_;
    total.fetch_add(1, std::memory_order_relaxed);
    reasons[index].fetch_add(1, std::memory_order_relaxed);
}

void PerformanceCounters::record_hle(std::string_view subsystem,
                                     std::uint64_t nanoseconds) {
    if (!enabled())
        return;
    std::lock_guard lock{hle_mutex_};
    auto [found, inserted] =
        hle_subsystems_.try_emplace(std::string{subsystem});
    auto& stats = found->second;
    if (inserted)
        stats.subsystem = subsystem;
    ++stats.calls;
    stats.nanoseconds += nanoseconds;
}

void PerformanceCounters::record_fork() {
    add_if_enabled(forks_, enabled_);
}

void PerformanceCounters::record_exec() {
    add_if_enabled(execs_, enabled_);
}

void PerformanceCounters::record_abnormal_exit() {
    add_if_enabled(abnormal_exits_, enabled_);
}

void PerformanceCounters::record_fallback(PerfFallbackReason reason) {
    const auto index = static_cast<std::size_t>(reason);
    if (!enabled() || reason == PerfFallbackReason::None ||
        index >= fallback_reasons_.size()) {
        return;
    }
    fallback_reasons_[index].fetch_add(1, std::memory_order_relaxed);
}

PerformanceSnapshot PerformanceCounters::snapshot() const {
    PerformanceSnapshot result;
    result.jit_instances = jit_instances_.load(std::memory_order_relaxed);
    result.jit_live_instances =
        jit_live_instances_.load(std::memory_order_relaxed);
    result.jit_live_peak_instances =
        jit_live_peak_instances_.load(std::memory_order_relaxed);
    result.jit_creation_nanoseconds =
        jit_creation_nanoseconds_.load(std::memory_order_relaxed);
    result.jit_code_cache_bytes =
        jit_code_cache_current_bytes_.load(std::memory_order_relaxed);
    result.jit_code_cache_peak_bytes =
        jit_code_cache_peak_bytes_.load(std::memory_order_relaxed);
    result.translation_blocks =
        translation_blocks_.load(std::memory_order_relaxed);
    result.cpu_executions = cpu_executions_.load(std::memory_order_relaxed);
    result.cpu_ticks = cpu_ticks_.load(std::memory_order_relaxed);
    result.svc_calls = svc_calls_.load(std::memory_order_relaxed);
    result.page_misses = page_misses_.load(std::memory_order_relaxed);
    result.page_faults = page_faults_.load(std::memory_order_relaxed);
    result.draws = draws_.load(std::memory_order_relaxed);
    result.submits = submits_.load(std::memory_order_relaxed);
    result.fence_waits = fence_waits_.load(std::memory_order_relaxed);
    result.fence_wait_nanoseconds =
        fence_wait_nanoseconds_.load(std::memory_order_relaxed);
    result.upload_bytes = upload_bytes_.load(std::memory_order_relaxed);
    result.readback_bytes = readback_bytes_.load(std::memory_order_relaxed);
    result.host_fills = host_fills_.load(std::memory_order_relaxed);
    result.host_copies = host_copies_.load(std::memory_order_relaxed);
    result.native_presents =
        native_presents_.load(std::memory_order_relaxed);
    result.cpu_present_fallbacks =
        cpu_present_fallbacks_.load(std::memory_order_relaxed);
    result.cpu_map_reads = cpu_map_reads_.load(std::memory_order_relaxed);
    result.cpu_map_writes =
        cpu_map_writes_.load(std::memory_order_relaxed);
    result.forks = forks_.load(std::memory_order_relaxed);
    result.execs = execs_.load(std::memory_order_relaxed);
    result.abnormal_exits =
        abnormal_exits_.load(std::memory_order_relaxed);
    for (std::size_t index = 0; index < perf_cpu_map_reason_count; ++index) {
        result.cpu_map_read_reasons[index] =
            cpu_map_read_reasons_[index].load(std::memory_order_relaxed);
        result.cpu_map_write_reasons[index] =
            cpu_map_write_reasons_[index].load(std::memory_order_relaxed);
    }
    for (std::size_t index = 0; index < perf_surface_kind_count; ++index) {
        result.surface_upload_bytes[index] =
            surface_upload_bytes_[index].load(std::memory_order_relaxed);
        result.surface_readback_bytes[index] =
            surface_readback_bytes_[index].load(std::memory_order_relaxed);
    }
    for (std::size_t index = 0; index < fallback_reasons_.size(); ++index) {
        result.fallback_reasons[index] =
            fallback_reasons_[index].load(std::memory_order_relaxed);
    }
    {
        std::lock_guard lock{hle_mutex_};
        result.hle_subsystems.reserve(hle_subsystems_.size());
        for (const auto& [name, stats] : hle_subsystems_) {
            static_cast<void>(name);
            result.hle_subsystems.push_back(stats);
        }
    }
    return result;
}

PerformanceCounters& performance_counters() {
    static PerformanceCounters counters;
    return counters;
}

std::string_view perf_fallback_reason_name(PerfFallbackReason reason) {
    switch (reason) {
    case PerfFallbackReason::None: return "none";
    case PerfFallbackReason::VulkanUnavailable: return "vulkan-unavailable";
    case PerfFallbackReason::InvalidTarget: return "invalid-target";
    case PerfFallbackReason::UnsupportedPrimitive:
        return "unsupported-primitive";
    case PerfFallbackReason::InvalidVertex: return "invalid-vertex";
    case PerfFallbackReason::UnsupportedBlend: return "unsupported-blend";
    case PerfFallbackReason::PipelineUnavailable:
        return "pipeline-unavailable";
    case PerfFallbackReason::TargetBusy: return "target-busy";
    case PerfFallbackReason::BackendFailure: return "backend-failure";
    case PerfFallbackReason::Count: break;
    }
    return "unknown";
}

std::string_view perf_cpu_map_reason_name(PerfCpuMapReason reason) {
    switch (reason) {
    case PerfCpuMapReason::Internal: return "internal";
    case PerfCpuMapReason::HostUpload: return "host-upload";
    case PerfCpuMapReason::GpuReadback: return "gpu-readback";
    case PerfCpuMapReason::CoreSurface: return "core-surface";
    case PerfCpuMapReason::SoftwareFallback: return "software-fallback";
    case PerfCpuMapReason::DeferredDisplayRead:
        return "deferred-display-read";
    case PerfCpuMapReason::NativePresent: return "native-present";
    case PerfCpuMapReason::Count: break;
    }
    return "unknown";
}

std::string_view perf_surface_kind_name(PerfSurfaceKind kind) {
    switch (kind) {
    case PerfSurfaceKind::Unknown: return "unknown";
    case PerfSurfaceKind::CoreSurface: return "core-surface";
    case PerfSurfaceKind::Scanout: return "scanout";
    case PerfSurfaceKind::GlesRenderTarget: return "gles-target";
    case PerfSurfaceKind::Count: break;
    }
    return "unknown";
}

std::string format_performance_summary(
    const PerformanceSnapshot& snapshot) {
    std::uint64_t fallback_total = 0;
    for (const auto count : snapshot.fallback_reasons)
        fallback_total += count;

    std::ostringstream text;
    text << "[perf] jit=" << snapshot.jit_instances
         << " jit-live=" << snapshot.jit_live_instances
         << " jit-live-peak=" << snapshot.jit_live_peak_instances
         << " jit-create-ns=" << snapshot.jit_creation_nanoseconds
         << " jit-cache-bytes=" << snapshot.jit_code_cache_bytes
         << " jit-cache-peak-bytes="
         << snapshot.jit_code_cache_peak_bytes
         << " translation-blocks=" << snapshot.translation_blocks
         << " cpu-exec=" << snapshot.cpu_executions
         << " cpu-ticks=" << snapshot.cpu_ticks
         << " svc=" << snapshot.svc_calls
         << " page-misses=" << snapshot.page_misses
         << " page-faults=" << snapshot.page_faults
         << " draw=" << snapshot.draws
         << " submit=" << snapshot.submits
         << " fence-wait=" << snapshot.fence_waits
         << " fence-wait-ns=" << snapshot.fence_wait_nanoseconds
         << " upload-bytes=" << snapshot.upload_bytes
         << " readback-bytes=" << snapshot.readback_bytes
         << " host-fill=" << snapshot.host_fills
         << " host-copy=" << snapshot.host_copies
         << " native-present=" << snapshot.native_presents
         << " cpu-present-fallback=" << snapshot.cpu_present_fallbacks
         << " cpu-map-read=" << snapshot.cpu_map_reads
         << " cpu-map-write=" << snapshot.cpu_map_writes
         << " fork=" << snapshot.forks
         << " exec=" << snapshot.execs
         << " abnormal-exit=" << snapshot.abnormal_exits
         << " fallback-total=" << fallback_total
         << " fallback-reasons=";
    bool first = true;
    for (std::size_t index = 1; index < snapshot.fallback_reasons.size();
         ++index) {
        const auto count = snapshot.fallback_reasons[index];
        if (count == 0)
            continue;
        if (!first)
            text << ',';
        first = false;
        text << perf_fallback_reason_name(
                    static_cast<PerfFallbackReason>(index))
             << ':' << count;
    }
    if (first)
        text << "none";
    text << " cpu-map-reasons=";
    first = true;
    for (std::size_t index = 0; index < perf_cpu_map_reason_count; ++index) {
        const auto reads = snapshot.cpu_map_read_reasons[index];
        const auto writes = snapshot.cpu_map_write_reasons[index];
        if (reads == 0 && writes == 0)
            continue;
        if (!first)
            text << ',';
        first = false;
        text << perf_cpu_map_reason_name(
                    static_cast<PerfCpuMapReason>(index))
             << ':' << reads << '/' << writes;
    }
    if (first)
        text << "none";
    text << " surface-transfer=";
    first = true;
    for (std::size_t index = 0; index < perf_surface_kind_count; ++index) {
        const auto uploads = snapshot.surface_upload_bytes[index];
        const auto readbacks = snapshot.surface_readback_bytes[index];
        if (uploads == 0 && readbacks == 0)
            continue;
        if (!first)
            text << ',';
        first = false;
        text << perf_surface_kind_name(
                    static_cast<PerfSurfaceKind>(index))
             << ':' << uploads << '/' << readbacks;
    }
    if (first)
        text << "none";
    text << " hle=";
    first = true;
    for (const auto& stats : snapshot.hle_subsystems) {
        if (!first)
            text << ',';
        first = false;
        text << stats.subsystem << ':' << stats.calls << '/'
             << stats.nanoseconds;
    }
    if (first)
        text << "none";
    return text.str();
}

} // namespace ilemu
