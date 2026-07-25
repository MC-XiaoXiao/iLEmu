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
    for (auto& counter : fallback_reasons_)
        counter.store(0, std::memory_order_relaxed);
    enabled_.store(enabled, std::memory_order_release);
}

void PerformanceCounters::record_jit() {
    add_if_enabled(jit_instances_, enabled_);
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

void PerformanceCounters::record_upload(std::uint64_t bytes) {
    add_if_enabled(upload_bytes_, enabled_, bytes);
}

void PerformanceCounters::record_readback(std::uint64_t bytes) {
    add_if_enabled(readback_bytes_, enabled_, bytes);
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
    for (std::size_t index = 0; index < fallback_reasons_.size(); ++index) {
        result.fallback_reasons[index] =
            fallback_reasons_[index].load(std::memory_order_relaxed);
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

std::string format_performance_summary(
    const PerformanceSnapshot& snapshot) {
    std::uint64_t fallback_total = 0;
    for (const auto count : snapshot.fallback_reasons)
        fallback_total += count;

    std::ostringstream text;
    text << "[perf] jit=" << snapshot.jit_instances
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
    return text.str();
}

} // namespace ilemu
