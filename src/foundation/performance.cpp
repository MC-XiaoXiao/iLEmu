#include "ilemu/performance.hpp"

#include <algorithm>
#include <iomanip>
#include <limits>
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

constexpr std::uint64_t ten_milliseconds = 10'000'000;
constexpr std::uint64_t one_sixtieth_second = 16'666'667;
constexpr std::uint64_t twenty_milliseconds = 20'000'000;
constexpr std::uint64_t one_thirtieth_second = 33'333'334;
constexpr std::uint64_t fifty_milliseconds = 50'000'000;
constexpr std::uint64_t one_hundred_milliseconds = 100'000'000;
constexpr std::uint64_t one_second = 1'000'000'000;
constexpr std::uint64_t ten_seconds = 10'000'000'000;

// Temporary frame-hitch diagnostic. Keep the ordered values out of the
// product snapshot and remove this probe after the exit/unlock stall is
// localized.
constexpr std::array diagnostic_sequence_kinds{
    PerfLatencyKind::DisplayMailbox,
    PerfLatencyKind::NativeMailbox,
    PerfLatencyKind::Acquire,
    PerfLatencyKind::QueuePresent,
    PerfLatencyKind::PresentReturn,
    PerfLatencyKind::PresentInterval,
    PerfLatencyKind::VsyncDueToCallback,
    PerfLatencyKind::VsyncCallbackToSwapEnd,
    PerfLatencyKind::VsyncSwapEndToGuestSubmit,
    PerfLatencyKind::VsyncDueToGuestSubmit,
};
std::array<std::vector<std::uint64_t>, diagnostic_sequence_kinds.size()>
    diagnostic_display_sequences;
constexpr std::size_t maximum_diagnostic_sequence_samples = 512;

struct DiagnosticFrameWork {
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
    std::array<std::uint64_t, perf_submit_reason_count> submit_reasons{};
    std::uint64_t hle_calls{};
    std::uint64_t hle_nanoseconds{};
    std::array<std::uint64_t, perf_diagnostic_graphics_hle_count>
        graphics_hle_calls{};
    std::array<std::uint64_t, perf_diagnostic_graphics_hle_count>
        graphics_hle_nanoseconds{};
};

struct DiagnosticDisplayFrame {
    std::uint64_t sequence{};
    std::uint32_t owner_process_id{};
    std::uint64_t vsync_sequence{};
    std::uint32_t framebuffer{};
    std::chrono::steady_clock::time_point submitted;
    std::chrono::steady_clock::time_point display_dequeued;
    std::chrono::steady_clock::time_point native_queued;
    std::chrono::steady_clock::time_point native_dequeued;
    std::chrono::steady_clock::time_point present_returned;
    char present_kind{'-'};
};

struct DiagnosticVsyncFrame {
    std::uint64_t sequence{};
    std::uint32_t process_id{};
    std::uint32_t framebuffer{};
    std::uint64_t display_sequence{};
    std::chrono::steady_clock::time_point due;
    std::chrono::steady_clock::time_point callback;
    std::chrono::steady_clock::time_point swap_end;
    std::chrono::steady_clock::time_point guest_submit;
    DiagnosticFrameWork work;
};

std::chrono::steady_clock::time_point diagnostic_window_started_at;
std::vector<DiagnosticDisplayFrame> diagnostic_display_frames;
std::vector<DiagnosticVsyncFrame> diagnostic_vsync_frames;
DiagnosticFrameWork diagnostic_previous_frame_work;
PerformanceSnapshot diagnostic_work_baseline;
std::map<std::string, HlePerformanceSnapshot, std::less<>>
    diagnostic_hle_baseline;

std::optional<std::size_t>
diagnostic_sequence_index(PerfLatencyKind kind) {
    const auto found =
        std::find(diagnostic_sequence_kinds.begin(),
                  diagnostic_sequence_kinds.end(), kind);
    if (found == diagnostic_sequence_kinds.end())
        return std::nullopt;
    return static_cast<std::size_t>(
        std::distance(diagnostic_sequence_kinds.begin(), found));
}

std::uint64_t diagnostic_delta(std::uint64_t current,
                               std::uint64_t baseline) {
    return current >= baseline ? current - baseline : 0;
}

DiagnosticFrameWork diagnostic_work_delta(
    const DiagnosticFrameWork& current,
    const DiagnosticFrameWork& previous) {
    auto result = DiagnosticFrameWork{
        diagnostic_delta(current.translation_blocks,
                         previous.translation_blocks),
        diagnostic_delta(current.cpu_executions, previous.cpu_executions),
        diagnostic_delta(current.cpu_ticks, previous.cpu_ticks),
        diagnostic_delta(current.svc_calls, previous.svc_calls),
        diagnostic_delta(current.page_misses, previous.page_misses),
        diagnostic_delta(current.page_faults, previous.page_faults),
        diagnostic_delta(current.draws, previous.draws),
        diagnostic_delta(current.submits, previous.submits),
        diagnostic_delta(current.fence_waits, previous.fence_waits),
        diagnostic_delta(current.fence_wait_nanoseconds,
                         previous.fence_wait_nanoseconds),
        {},
        diagnostic_delta(current.hle_calls, previous.hle_calls),
        diagnostic_delta(current.hle_nanoseconds,
                         previous.hle_nanoseconds),
    };
    for (std::size_t index = 0; index < result.submit_reasons.size();
         ++index) {
        result.submit_reasons[index] =
            diagnostic_delta(current.submit_reasons[index],
                             previous.submit_reasons[index]);
    }
    for (std::size_t index = 0; index < result.graphics_hle_calls.size();
         ++index) {
        result.graphics_hle_calls[index] =
            diagnostic_delta(current.graphics_hle_calls[index],
                             previous.graphics_hle_calls[index]);
        result.graphics_hle_nanoseconds[index] =
            diagnostic_delta(current.graphics_hle_nanoseconds[index],
                             previous.graphics_hle_nanoseconds[index]);
    }
    return result;
}

DiagnosticDisplayFrame*
diagnostic_display_frame(std::uint64_t sequence) {
    const auto found = std::find_if(
        diagnostic_display_frames.rbegin(),
        diagnostic_display_frames.rend(),
        [sequence](const auto& frame) {
            return frame.sequence == sequence;
        });
    return found == diagnostic_display_frames.rend()
               ? nullptr
               : &*found;
}

void append_diagnostic_timestamp(
    std::ostringstream& text,
    std::chrono::steady_clock::time_point timestamp) {
    if (timestamp == std::chrono::steady_clock::time_point{} ||
        diagnostic_window_started_at ==
            std::chrono::steady_clock::time_point{}) {
        text << '-';
        return;
    }
    text << std::chrono::duration_cast<std::chrono::microseconds>(
                timestamp - diagnostic_window_started_at)
                .count();
}

std::size_t latency_bucket(std::uint64_t nanoseconds) {
    if (nanoseconds < ten_milliseconds)
        return static_cast<std::size_t>(nanoseconds / 10'000);
    if (nanoseconds < one_hundred_milliseconds) {
        return 1000U + static_cast<std::size_t>(
                           (nanoseconds - ten_milliseconds) / 100'000);
    }
    if (nanoseconds < one_second) {
        return 1900U + static_cast<std::size_t>(
                           (nanoseconds - one_hundred_milliseconds) /
                           1'000'000);
    }
    if (nanoseconds < ten_seconds) {
        return 2800U + static_cast<std::size_t>(
                           (nanoseconds - one_second) / 10'000'000);
    }
    return 3700U;
}

std::uint64_t latency_bucket_upper_bound(std::size_t bucket,
                                         std::uint64_t maximum) {
    if (bucket < 1000U)
        return (bucket + 1U) * 10'000U;
    if (bucket < 1900U)
        return ten_milliseconds + (bucket - 999U) * 100'000U;
    if (bucket < 2800U) {
        return one_hundred_milliseconds +
               (bucket - 1899U) * 1'000'000U;
    }
    if (bucket < 3700U)
        return one_second + (bucket - 2799U) * 10'000'000U;
    return maximum;
}

std::uint64_t steady_nanoseconds(
    std::chrono::steady_clock::time_point timestamp) {
    if (timestamp == std::chrono::steady_clock::time_point{})
        return 0;
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            timestamp.time_since_epoch())
            .count());
}

void record_timestamp_span(std::atomic<std::uint64_t>& samples,
                           std::atomic<std::uint64_t>& first,
                           std::atomic<std::uint64_t>& last,
                           std::uint64_t timestamp) {
    samples.fetch_add(1, std::memory_order_relaxed);
    auto unset = std::uint64_t{};
    static_cast<void>(first.compare_exchange_strong(
        unset, timestamp, std::memory_order_relaxed,
        std::memory_order_relaxed));
    last.store(timestamp, std::memory_order_relaxed);
}

double frames_per_second(std::uint64_t frames, std::uint64_t first,
                         std::uint64_t last) {
    if (frames < 2 || last <= first)
        return 0.0;
    return static_cast<double>(frames - 1U) * 1'000'000'000.0 /
           static_cast<double>(last - first);
}

bool is_display_window_latency(PerfLatencyKind kind) {
    switch (kind) {
    case PerfLatencyKind::DisplayMailbox:
    case PerfLatencyKind::NativeMailbox:
    case PerfLatencyKind::Acquire:
    case PerfLatencyKind::QueuePresent:
    case PerfLatencyKind::PresentReturn:
    case PerfLatencyKind::PresentInterval:
    case PerfLatencyKind::VsyncDueToCallback:
    case PerfLatencyKind::VsyncCallbackToSwapEnd:
    case PerfLatencyKind::VsyncSwapEndToGuestSubmit:
    case PerfLatencyKind::VsyncDueToGuestSubmit:
        return true;
    case PerfLatencyKind::InputEnqueue:
    case PerfLatencyKind::DisplayPresent:
    case PerfLatencyKind::JitColdPath:
    case PerfLatencyKind::JitBlockCompile:
    case PerfLatencyKind::RuntimeDestructor:
    case PerfLatencyKind::GlesTargetRelease:
    case PerfLatencyKind::PosixSpawnTotal:
    case PerfLatencyKind::PosixSpawnDecode:
    case PerfLatencyKind::PosixSpawnFork:
    case PerfLatencyKind::PosixSpawnCreate:
    case PerfLatencyKind::ProcessFreshMemory:
    case PerfLatencyKind::ProcessCloneMemory:
    case PerfLatencyKind::ProcessCreateCpu:
    case PerfLatencyKind::ProcessCreateKernel:
    case PerfLatencyKind::ProcessInheritKernel:
    case PerfLatencyKind::ProcessInheritSpawnKernel:
    case PerfLatencyKind::ProcessConfigureRuntime:
    case PerfLatencyKind::SpawnMemoryClear:
    case PerfLatencyKind::SpawnImageLoad:
    case PerfLatencyKind::SpawnResetRuntime:
    case PerfLatencyKind::Count:
        return false;
    }
    return false;
}

} // namespace

void PerformanceCounters::reset(bool enabled) {
    enabled_.store(false, std::memory_order_relaxed);
    display_window_active_.store(false, std::memory_order_release);
    jit_instances_.store(0, std::memory_order_relaxed);
    jit_live_instances_.store(0, std::memory_order_relaxed);
    jit_live_peak_instances_.store(0, std::memory_order_relaxed);
    jit_creation_nanoseconds_.store(0, std::memory_order_relaxed);
    jit_block_compile_p95_nanoseconds_.store(0, std::memory_order_relaxed);
    jit_block_compile_p99_nanoseconds_.store(0, std::memory_order_relaxed);
    jit_code_cache_current_bytes_.store(0, std::memory_order_relaxed);
    jit_code_cache_peak_bytes_.store(0, std::memory_order_relaxed);
    jit_shared_invalidation_requests_.store(0, std::memory_order_relaxed);
    translation_blocks_.store(0, std::memory_order_relaxed);
    cpu_executions_.store(0, std::memory_order_relaxed);
    cpu_ticks_.store(0, std::memory_order_relaxed);
    svc_calls_.store(0, std::memory_order_relaxed);
    page_misses_.store(0, std::memory_order_relaxed);
    page_faults_.store(0, std::memory_order_relaxed);
    draws_.store(0, std::memory_order_relaxed);
    submits_.store(0, std::memory_order_relaxed);
    for (auto& counter : submit_reasons_)
        counter.store(0, std::memory_order_relaxed);
    fence_waits_.store(0, std::memory_order_relaxed);
    fence_wait_nanoseconds_.store(0, std::memory_order_relaxed);
    upload_bytes_.store(0, std::memory_order_relaxed);
    readback_bytes_.store(0, std::memory_order_relaxed);
    host_fills_.store(0, std::memory_order_relaxed);
    host_copies_.store(0, std::memory_order_relaxed);
    display_submissions_.store(0, std::memory_order_relaxed);
    display_first_nanoseconds_.store(0, std::memory_order_relaxed);
    display_last_nanoseconds_.store(0, std::memory_order_relaxed);
    display_mailbox_coalesced_.store(0, std::memory_order_relaxed);
    display_vsync_budget_cuts_.store(0, std::memory_order_relaxed);
    display_vsync_budget_saved_ticks_.store(0, std::memory_order_relaxed);
    sdl_idle_waits_.store(0, std::memory_order_relaxed);
    native_present_attempts_.store(0, std::memory_order_relaxed);
    native_present_mailbox_coalesced_.store(0,
                                            std::memory_order_relaxed);
    native_present_skipped_.store(0, std::memory_order_relaxed);
    native_present_failures_.store(0, std::memory_order_relaxed);
    native_presents_.store(0, std::memory_order_relaxed);
    present_first_nanoseconds_.store(0, std::memory_order_relaxed);
    present_last_nanoseconds_.store(0, std::memory_order_relaxed);
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
    for (auto& histogram : latencies_) {
        for (auto& bucket : histogram.buckets)
            bucket.store(0, std::memory_order_relaxed);
        histogram.samples.store(0, std::memory_order_relaxed);
        histogram.maximum_nanoseconds.store(0, std::memory_order_relaxed);
        histogram.over_16_7ms.store(0, std::memory_order_relaxed);
        histogram.over_20ms.store(0, std::memory_order_relaxed);
        histogram.over_33_3ms.store(0, std::memory_order_relaxed);
        histogram.over_50ms.store(0, std::memory_order_relaxed);
    }
    {
        std::lock_guard lock{vsync_timeline_mutex_};
        vsync_timelines_.clear();
    }
    {
        std::lock_guard lock{jit_cache_slots_mutex_};
        jit_cache_slots_.clear();
    }
    {
        std::lock_guard lock{hle_mutex_};
        hle_subsystems_.clear();
    }
    diagnostic_hle_calls_.store(0, std::memory_order_relaxed);
    diagnostic_hle_nanoseconds_.store(0, std::memory_order_relaxed);
    for (auto& counter : diagnostic_graphics_hle_calls_)
        counter.store(0, std::memory_order_relaxed);
    for (auto& counter : diagnostic_graphics_hle_nanoseconds_)
        counter.store(0, std::memory_order_relaxed);
    {
        std::lock_guard lock{display_window_mutex_};
        reset_display_window_locked();
    }
    enabled_.store(enabled, std::memory_order_release);
}

void PerformanceCounters::reset_display_window_locked() {
    display_window_snapshot_ = PerformanceSnapshot{};
    for (auto& histogram : display_window_latencies_)
        histogram = DisplayWindowLatencyHistogram{};
    for (auto& sequence : diagnostic_display_sequences)
        sequence.clear();
    diagnostic_window_started_at = {};
    diagnostic_display_frames.clear();
    diagnostic_vsync_frames.clear();
    diagnostic_previous_frame_work = {};
}

bool PerformanceCounters::begin_display_window() {
    if (!enabled())
        return false;
    std::lock_guard lock{display_window_mutex_};
    if (display_window_active_.load(std::memory_order_relaxed))
        return false;
    reset_display_window_locked();
    diagnostic_window_started_at = std::chrono::steady_clock::now();
    diagnostic_previous_frame_work = {
        translation_blocks_.load(std::memory_order_relaxed),
        cpu_executions_.load(std::memory_order_relaxed),
        cpu_ticks_.load(std::memory_order_relaxed),
        svc_calls_.load(std::memory_order_relaxed),
        page_misses_.load(std::memory_order_relaxed),
        page_faults_.load(std::memory_order_relaxed),
        draws_.load(std::memory_order_relaxed),
        submits_.load(std::memory_order_relaxed),
        fence_waits_.load(std::memory_order_relaxed),
        fence_wait_nanoseconds_.load(std::memory_order_relaxed),
    };
    for (std::size_t index = 0;
         index < diagnostic_previous_frame_work.submit_reasons.size();
         ++index) {
        diagnostic_previous_frame_work.submit_reasons[index] =
            submit_reasons_[index].load(std::memory_order_relaxed);
    }
    diagnostic_previous_frame_work.hle_calls =
        diagnostic_hle_calls_.load(std::memory_order_relaxed);
    diagnostic_previous_frame_work.hle_nanoseconds =
        diagnostic_hle_nanoseconds_.load(std::memory_order_relaxed);
    for (std::size_t index = 0;
         index < diagnostic_previous_frame_work.graphics_hle_calls.size();
         ++index) {
        diagnostic_previous_frame_work.graphics_hle_calls[index] =
            diagnostic_graphics_hle_calls_[index].load(
                std::memory_order_relaxed);
        diagnostic_previous_frame_work.graphics_hle_nanoseconds[index] =
            diagnostic_graphics_hle_nanoseconds_[index].load(
                std::memory_order_relaxed);
    }
    diagnostic_work_baseline.jit_instances =
        jit_instances_.load(std::memory_order_relaxed);
    diagnostic_work_baseline.jit_creation_nanoseconds =
        jit_creation_nanoseconds_.load(std::memory_order_relaxed);
    diagnostic_work_baseline.translation_blocks =
        translation_blocks_.load(std::memory_order_relaxed);
    diagnostic_work_baseline.cpu_executions =
        cpu_executions_.load(std::memory_order_relaxed);
    diagnostic_work_baseline.cpu_ticks =
        cpu_ticks_.load(std::memory_order_relaxed);
    diagnostic_work_baseline.svc_calls =
        svc_calls_.load(std::memory_order_relaxed);
    diagnostic_work_baseline.draws =
        draws_.load(std::memory_order_relaxed);
    diagnostic_work_baseline.submits =
        submits_.load(std::memory_order_relaxed);
    for (std::size_t index = 0;
         index < diagnostic_work_baseline.submit_reasons.size(); ++index) {
        diagnostic_work_baseline.submit_reasons[index] =
            submit_reasons_[index].load(std::memory_order_relaxed);
    }
    diagnostic_work_baseline.fence_waits =
        fence_waits_.load(std::memory_order_relaxed);
    diagnostic_work_baseline.fence_wait_nanoseconds =
        fence_wait_nanoseconds_.load(std::memory_order_relaxed);
    diagnostic_work_baseline.upload_bytes =
        upload_bytes_.load(std::memory_order_relaxed);
    diagnostic_work_baseline.readback_bytes =
        readback_bytes_.load(std::memory_order_relaxed);
    diagnostic_work_baseline.host_fills =
        host_fills_.load(std::memory_order_relaxed);
    diagnostic_work_baseline.host_copies =
        host_copies_.load(std::memory_order_relaxed);
    diagnostic_work_baseline.cpu_map_reads =
        cpu_map_reads_.load(std::memory_order_relaxed);
    diagnostic_work_baseline.cpu_map_writes =
        cpu_map_writes_.load(std::memory_order_relaxed);
    {
        std::lock_guard hle_lock{hle_mutex_};
        diagnostic_hle_baseline = hle_subsystems_;
    }
    display_window_active_.store(true, std::memory_order_release);
    return true;
}

std::optional<PerformanceSnapshot>
PerformanceCounters::end_display_window() {
    std::lock_guard lock{display_window_mutex_};
    if (!display_window_active_.load(std::memory_order_relaxed))
        return std::nullopt;
    display_window_active_.store(false, std::memory_order_release);

    auto result = display_window_snapshot_;
    for (std::size_t index = 0; index < display_window_latencies_.size();
         ++index) {
        const auto& histogram = display_window_latencies_[index];
        auto& latency = result.latencies[index];
        latency.samples = histogram.samples;
        latency.maximum_nanoseconds = histogram.maximum_nanoseconds;
        latency.over_16_7ms = histogram.over_16_7ms;
        latency.over_20ms = histogram.over_20ms;
        latency.over_33_3ms = histogram.over_33_3ms;
        latency.over_50ms = histogram.over_50ms;
        if (latency.samples == 0)
            continue;
        const auto percentile = [&](std::uint64_t numerator) {
            const auto rank =
                (latency.samples * numerator + 99U) / 100U;
            std::uint64_t cumulative{};
            for (std::size_t bucket = 0;
                 bucket < histogram.buckets.size(); ++bucket) {
                cumulative += histogram.buckets[bucket];
                if (cumulative >= rank) {
                    return latency_bucket_upper_bound(
                        bucket, latency.maximum_nanoseconds);
                }
            }
            return latency.maximum_nanoseconds;
        };
        latency.p50_nanoseconds = percentile(50);
        latency.p95_nanoseconds = percentile(95);
        latency.p99_nanoseconds = percentile(99);
    }
    result.jit_instances = diagnostic_delta(
        jit_instances_.load(std::memory_order_relaxed),
        diagnostic_work_baseline.jit_instances);
    result.jit_creation_nanoseconds = diagnostic_delta(
        jit_creation_nanoseconds_.load(std::memory_order_relaxed),
        diagnostic_work_baseline.jit_creation_nanoseconds);
    result.translation_blocks = diagnostic_delta(
        translation_blocks_.load(std::memory_order_relaxed),
        diagnostic_work_baseline.translation_blocks);
    result.cpu_executions = diagnostic_delta(
        cpu_executions_.load(std::memory_order_relaxed),
        diagnostic_work_baseline.cpu_executions);
    result.cpu_ticks = diagnostic_delta(
        cpu_ticks_.load(std::memory_order_relaxed),
        diagnostic_work_baseline.cpu_ticks);
    result.svc_calls = diagnostic_delta(
        svc_calls_.load(std::memory_order_relaxed),
        diagnostic_work_baseline.svc_calls);
    result.draws = diagnostic_delta(
        draws_.load(std::memory_order_relaxed),
        diagnostic_work_baseline.draws);
    result.submits = diagnostic_delta(
        submits_.load(std::memory_order_relaxed),
        diagnostic_work_baseline.submits);
    for (std::size_t index = 0; index < result.submit_reasons.size();
         ++index) {
        result.submit_reasons[index] = diagnostic_delta(
            submit_reasons_[index].load(std::memory_order_relaxed),
            diagnostic_work_baseline.submit_reasons[index]);
    }
    result.fence_waits = diagnostic_delta(
        fence_waits_.load(std::memory_order_relaxed),
        diagnostic_work_baseline.fence_waits);
    result.fence_wait_nanoseconds = diagnostic_delta(
        fence_wait_nanoseconds_.load(std::memory_order_relaxed),
        diagnostic_work_baseline.fence_wait_nanoseconds);
    result.upload_bytes = diagnostic_delta(
        upload_bytes_.load(std::memory_order_relaxed),
        diagnostic_work_baseline.upload_bytes);
    result.readback_bytes = diagnostic_delta(
        readback_bytes_.load(std::memory_order_relaxed),
        diagnostic_work_baseline.readback_bytes);
    result.host_fills = diagnostic_delta(
        host_fills_.load(std::memory_order_relaxed),
        diagnostic_work_baseline.host_fills);
    result.host_copies = diagnostic_delta(
        host_copies_.load(std::memory_order_relaxed),
        diagnostic_work_baseline.host_copies);
    result.cpu_map_reads = diagnostic_delta(
        cpu_map_reads_.load(std::memory_order_relaxed),
        diagnostic_work_baseline.cpu_map_reads);
    result.cpu_map_writes = diagnostic_delta(
        cpu_map_writes_.load(std::memory_order_relaxed),
        diagnostic_work_baseline.cpu_map_writes);
    {
        std::lock_guard hle_lock{hle_mutex_};
        for (const auto& [name, current] : hle_subsystems_) {
            const auto baseline = diagnostic_hle_baseline.find(name);
            const auto baseline_calls =
                baseline == diagnostic_hle_baseline.end()
                    ? 0
                    : baseline->second.calls;
            const auto baseline_nanoseconds =
                baseline == diagnostic_hle_baseline.end()
                    ? 0
                    : baseline->second.nanoseconds;
            const auto calls =
                diagnostic_delta(current.calls, baseline_calls);
            const auto nanoseconds =
                diagnostic_delta(current.nanoseconds, baseline_nanoseconds);
            if (calls != 0 || nanoseconds != 0) {
                result.hle_subsystems.push_back(
                    HlePerformanceSnapshot{name, calls, nanoseconds});
            }
        }
    }
    return result;
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

void PerformanceCounters::record_jit_block_compile(
    std::uint64_t nanoseconds) {
    // Keep this small history even when --perf-summary is not requested: the
    // scheduler uses it to protect the next guest deadline from one
    // non-preemptible background translation.
    record_global_latency(PerfLatencyKind::JitBlockCompile, nanoseconds);
    refresh_jit_block_compile_percentiles();
}

void PerformanceCounters::refresh_jit_block_compile_percentiles() {
    const auto &histogram =
        latencies_[static_cast<std::size_t>(PerfLatencyKind::JitBlockCompile)];
    const auto samples = histogram.samples.load(std::memory_order_relaxed);
    if (samples == 0) return;
    const auto percentile = [&](std::uint64_t numerator) {
        const auto rank = (samples * numerator + 99U) / 100U;
        std::uint64_t cumulative{};
        for (std::size_t bucket = 0; bucket < histogram.buckets.size();
             ++bucket) {
            cumulative += histogram.buckets[bucket].load(
                std::memory_order_relaxed);
            if (cumulative >= rank) {
                return latency_bucket_upper_bound(
                    bucket,
                    histogram.maximum_nanoseconds.load(
                        std::memory_order_relaxed));
            }
        }
        return histogram.maximum_nanoseconds.load(std::memory_order_relaxed);
    };
    jit_block_compile_p95_nanoseconds_.store(percentile(95),
                                             std::memory_order_relaxed);
    jit_block_compile_p99_nanoseconds_.store(percentile(99),
                                             std::memory_order_relaxed);
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
    std::uint32_t process_id, std::uint32_t slot,
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
    std::lock_guard lock{jit_cache_slots_mutex_};
    auto [found, inserted] =
        jit_cache_slots_.try_emplace({process_id, slot});
    auto& usage = found->second;
    if (inserted) {
        usage.process_id = process_id;
        usage.slot = slot;
    }
    usage.current_bytes = current_bytes;
    usage.peak_bytes = std::max(usage.peak_bytes, current_bytes);
}

void PerformanceCounters::record_jit_shared_invalidation() {
    if (!enabled()) return;
    jit_shared_invalidation_requests_.fetch_add(1,
                                                std::memory_order_relaxed);
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

void PerformanceCounters::record_submit(PerfSubmitReason reason) {
    add_if_enabled(submits_, enabled_);
    const auto index = static_cast<std::size_t>(reason);
    if (enabled() && index < submit_reasons_.size())
        submit_reasons_[index].fetch_add(1, std::memory_order_relaxed);
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

void PerformanceCounters::record_display_submission(
    std::uint64_t frame_sequence, std::uint32_t owner_process_id,
    std::chrono::steady_clock::time_point submitted_at) {
    if (!enabled())
        return;
    const auto timestamp = steady_nanoseconds(submitted_at);
    if (timestamp == 0)
        return;
    record_timestamp_span(display_submissions_, display_first_nanoseconds_,
                          display_last_nanoseconds_, timestamp);
    if (!display_window_active_.load(std::memory_order_acquire))
        return;
    std::lock_guard lock{display_window_mutex_};
    if (!display_window_active_.load(std::memory_order_relaxed))
        return;
    auto& window = display_window_snapshot_;
    ++window.display_submissions;
    if (window.display_first_nanoseconds == 0)
        window.display_first_nanoseconds = timestamp;
    window.display_last_nanoseconds = timestamp;
    if (frame_sequence != 0 &&
        diagnostic_display_frames.size() <
            maximum_diagnostic_sequence_samples) {
        DiagnosticDisplayFrame frame;
        frame.sequence = frame_sequence;
        frame.owner_process_id = owner_process_id;
        frame.submitted = submitted_at;
        diagnostic_display_frames.push_back(frame);
    }
}

void PerformanceCounters::record_diagnostic_display_dequeue(
    std::uint64_t frame_sequence,
    std::chrono::steady_clock::time_point dequeued_at) {
    if (!enabled() || frame_sequence == 0 ||
        !display_window_active_.load(std::memory_order_acquire)) {
        return;
    }
    std::lock_guard lock{display_window_mutex_};
    if (!display_window_active_.load(std::memory_order_relaxed))
        return;
    if (auto* frame = diagnostic_display_frame(frame_sequence))
        frame->display_dequeued = dequeued_at;
}

void PerformanceCounters::record_diagnostic_native_queue(
    std::uint64_t frame_sequence,
    std::chrono::steady_clock::time_point queued_at) {
    if (!enabled() || frame_sequence == 0 ||
        !display_window_active_.load(std::memory_order_acquire)) {
        return;
    }
    std::lock_guard lock{display_window_mutex_};
    if (!display_window_active_.load(std::memory_order_relaxed))
        return;
    if (auto* frame = diagnostic_display_frame(frame_sequence))
        frame->native_queued = queued_at;
}

void PerformanceCounters::record_diagnostic_native_dequeue(
    std::uint64_t frame_sequence,
    std::chrono::steady_clock::time_point dequeued_at) {
    if (!enabled() || frame_sequence == 0 ||
        !display_window_active_.load(std::memory_order_acquire)) {
        return;
    }
    std::lock_guard lock{display_window_mutex_};
    if (!display_window_active_.load(std::memory_order_relaxed))
        return;
    if (auto* frame = diagnostic_display_frame(frame_sequence))
        frame->native_dequeued = dequeued_at;
}

void PerformanceCounters::add_display_window_counter(
    std::uint64_t PerformanceSnapshot::*counter, std::uint64_t value) {
    if (!display_window_active_.load(std::memory_order_acquire))
        return;
    std::lock_guard lock{display_window_mutex_};
    if (!display_window_active_.load(std::memory_order_relaxed))
        return;
    display_window_snapshot_.*counter += value;
}

void PerformanceCounters::record_display_mailbox_coalesced() {
    if (!enabled())
        return;
    display_mailbox_coalesced_.fetch_add(1, std::memory_order_relaxed);
    add_display_window_counter(
        &PerformanceSnapshot::display_mailbox_coalesced);
}

void PerformanceCounters::record_display_vsync_budget(
    std::uint64_t original_ticks, std::uint64_t limited_ticks) {
    if (!enabled() || limited_ticks >= original_ticks)
        return;
    display_vsync_budget_cuts_.fetch_add(1, std::memory_order_relaxed);
    display_vsync_budget_saved_ticks_.fetch_add(
        original_ticks - limited_ticks, std::memory_order_relaxed);
    add_display_window_counter(
        &PerformanceSnapshot::display_vsync_budget_cuts);
    add_display_window_counter(
        &PerformanceSnapshot::display_vsync_budget_saved_ticks,
        original_ticks - limited_ticks);
}

void PerformanceCounters::record_sdl_idle_wait() {
    if (!enabled())
        return;
    sdl_idle_waits_.fetch_add(1, std::memory_order_relaxed);
}

void PerformanceCounters::record_native_present_attempt() {
    if (!enabled())
        return;
    native_present_attempts_.fetch_add(1, std::memory_order_relaxed);
    add_display_window_counter(&PerformanceSnapshot::native_present_attempts);
}

void PerformanceCounters::record_native_present_mailbox_coalesced() {
    if (!enabled())
        return;
    native_present_mailbox_coalesced_.fetch_add(
        1, std::memory_order_relaxed);
    add_display_window_counter(
        &PerformanceSnapshot::native_present_mailbox_coalesced);
}

void PerformanceCounters::record_native_present_skipped() {
    if (!enabled())
        return;
    native_present_skipped_.fetch_add(1, std::memory_order_relaxed);
    add_display_window_counter(&PerformanceSnapshot::native_present_skipped);
}

void PerformanceCounters::record_native_present_failure() {
    if (!enabled())
        return;
    native_present_failures_.fetch_add(1, std::memory_order_relaxed);
    add_display_window_counter(&PerformanceSnapshot::native_present_failures);
}

void PerformanceCounters::record_present_completion(
    bool native, std::uint64_t frame_sequence,
    std::chrono::steady_clock::time_point submitted_at) {
    if (!enabled())
        return;
    std::lock_guard timeline_lock{present_timeline_mutex_};
    const auto returned_at = std::chrono::steady_clock::now();
    const auto returned_nanoseconds = steady_nanoseconds(returned_at);
    auto& global_count =
        native ? native_presents_ : cpu_present_fallbacks_;
    global_count.fetch_add(1, std::memory_order_relaxed);
    if (present_first_nanoseconds_.load(std::memory_order_relaxed) == 0) {
        present_first_nanoseconds_.store(returned_nanoseconds,
                                         std::memory_order_relaxed);
    }
    const auto previous =
        present_last_nanoseconds_.load(std::memory_order_relaxed);
    present_last_nanoseconds_.store(returned_nanoseconds,
                                    std::memory_order_relaxed);
    if (previous != 0)
        record_global_latency(PerfLatencyKind::PresentInterval,
                              returned_nanoseconds - previous);
    const auto submitted_nanoseconds = steady_nanoseconds(submitted_at);
    if (submitted_nanoseconds != 0 &&
        returned_nanoseconds > submitted_nanoseconds) {
        record_global_latency(PerfLatencyKind::PresentReturn,
                              returned_nanoseconds - submitted_nanoseconds);
    }

    if (!display_window_active_.load(std::memory_order_acquire))
        return;
    std::lock_guard window_lock{display_window_mutex_};
    if (!display_window_active_.load(std::memory_order_relaxed))
        return;
    auto& window = display_window_snapshot_;
    if (native)
        ++window.native_presents;
    else
        ++window.cpu_present_fallbacks;
    if (window.present_first_nanoseconds == 0)
        window.present_first_nanoseconds = returned_nanoseconds;
    const auto window_previous = window.present_last_nanoseconds;
    window.present_last_nanoseconds = returned_nanoseconds;
    if (window_previous != 0) {
        record_display_window_latency_locked(
            PerfLatencyKind::PresentInterval,
            returned_nanoseconds - window_previous);
    }
    if (submitted_nanoseconds != 0 &&
        returned_nanoseconds > submitted_nanoseconds) {
        record_display_window_latency_locked(
            PerfLatencyKind::PresentReturn,
            returned_nanoseconds - submitted_nanoseconds);
    }
    if (auto* frame = diagnostic_display_frame(frame_sequence)) {
        frame->present_returned = returned_at;
        frame->present_kind = native ? 'n' : 'c';
    }
}

void PerformanceCounters::record_native_present(
    std::uint64_t frame_sequence,
    std::chrono::steady_clock::time_point submitted_at) {
    record_present_completion(true, frame_sequence, submitted_at);
}

void PerformanceCounters::record_cpu_present_fallback(
    std::uint64_t frame_sequence,
    std::chrono::steady_clock::time_point submitted_at) {
    record_present_completion(false, frame_sequence, submitted_at);
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

void PerformanceCounters::record_vsync_due(std::uint32_t process_id,
                                           std::uint32_t framebuffer,
                                           std::uint64_t sequence) {
    if (!enabled() || process_id == 0 || framebuffer == 0 || sequence == 0)
        return;
    std::lock_guard lock{vsync_timeline_mutex_};
    auto& timelines = vsync_timelines_[{process_id, framebuffer}];
    constexpr std::size_t maximum_pending_timelines = 8;
    if (timelines.size() == maximum_pending_timelines)
        timelines.pop_front();
    timelines.push_back(
        VsyncTimeline{sequence, std::chrono::steady_clock::now(), {}, {}});
}

void PerformanceCounters::record_vsync_callback(
    std::uint32_t process_id, std::uint32_t framebuffer,
    std::uint64_t sequence) {
    if (!enabled() || process_id == 0 || framebuffer == 0)
        return;
    const auto now = std::chrono::steady_clock::now();
    std::optional<std::uint64_t> due_to_callback;
    {
        std::lock_guard lock{vsync_timeline_mutex_};
        const auto found = vsync_timelines_.find({process_id, framebuffer});
        if (found == vsync_timelines_.end())
            return;
        auto timeline = found->second.end();
        if (sequence != 0) {
            const auto matching = std::find_if(
                found->second.rbegin(), found->second.rend(),
                [sequence](const auto& candidate) {
                    return candidate.sequence == sequence;
                });
            if (matching != found->second.rend()) {
                timeline = matching.base();
                --timeline;
            }
        } else {
            const auto pending = std::find_if(
                found->second.rbegin(), found->second.rend(),
                [](const auto& candidate) {
                    return candidate.callback ==
                           std::chrono::steady_clock::time_point{};
                });
            if (pending != found->second.rend()) {
                timeline = pending.base();
                --timeline;
            }
        }
        if (timeline == found->second.end())
            return;
        // A newer firmware callback supersedes an older callback that did not
        // submit a frame. Discard those unconsumed prefixes so a later
        // input-driven SwapEnd cannot be paired with a stale VSync.
        found->second.erase(found->second.begin(), timeline);
        auto& current = found->second.front();
        current.callback = now;
        if (current.due != std::chrono::steady_clock::time_point{} &&
            now >= current.due) {
            due_to_callback = static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    now - current.due)
                    .count());
        }
    }
    if (due_to_callback)
        record_latency(PerfLatencyKind::VsyncDueToCallback, *due_to_callback);
}

void PerformanceCounters::record_vsync_swap_end(
    std::uint32_t process_id, std::uint32_t framebuffer) {
    if (!enabled() || process_id == 0 || framebuffer == 0)
        return;
    const auto now = std::chrono::steady_clock::now();
    std::optional<std::uint64_t> callback_to_swap_end;
    {
        std::lock_guard lock{vsync_timeline_mutex_};
        const auto found = vsync_timelines_.find({process_id, framebuffer});
        if (found == vsync_timelines_.end())
            return;
        const auto timeline = std::find_if(
            found->second.rbegin(), found->second.rend(), [](const auto& item) {
                return item.callback !=
                           std::chrono::steady_clock::time_point{} &&
                       item.swap_end ==
                           std::chrono::steady_clock::time_point{};
            });
        if (timeline == found->second.rend())
            return;
        timeline->swap_end = now;
        if (now >= timeline->callback) {
            callback_to_swap_end = static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    now - timeline->callback)
                    .count());
        }
    }
    if (callback_to_swap_end) {
        record_latency(PerfLatencyKind::VsyncCallbackToSwapEnd,
                       *callback_to_swap_end);
    }
}

void PerformanceCounters::record_vsync_guest_submit(
    std::uint32_t process_id, std::uint32_t framebuffer) {
    if (!enabled() || process_id == 0 || framebuffer == 0)
        return;
    const auto now = std::chrono::steady_clock::now();
    std::optional<std::uint64_t> swap_end_to_submit;
    std::optional<std::uint64_t> due_to_submit;
    std::optional<VsyncTimeline> completed_timeline;
    {
        std::lock_guard lock{vsync_timeline_mutex_};
        const auto found = vsync_timelines_.find({process_id, framebuffer});
        if (found == vsync_timelines_.end())
            return;
        const auto timeline = std::find_if(
            found->second.rbegin(), found->second.rend(), [](const auto& item) {
                return item.swap_end !=
                       std::chrono::steady_clock::time_point{};
            });
        if (timeline == found->second.rend())
            return;
        if (now >= timeline->swap_end) {
            swap_end_to_submit = static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    now - timeline->swap_end)
                    .count());
        }
        if (timeline->due != std::chrono::steady_clock::time_point{} &&
            now >= timeline->due) {
            due_to_submit = static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    now - timeline->due)
                    .count());
        }
        completed_timeline = *timeline;
        const auto erase_end = timeline.base();
        found->second.erase(found->second.begin(), erase_end);
        if (found->second.empty())
            vsync_timelines_.erase(found);
    }
    if (swap_end_to_submit) {
        record_latency(PerfLatencyKind::VsyncSwapEndToGuestSubmit,
                       *swap_end_to_submit);
    }
    if (due_to_submit)
        record_latency(PerfLatencyKind::VsyncDueToGuestSubmit, *due_to_submit);

    if (!completed_timeline ||
        !display_window_active_.load(std::memory_order_acquire)) {
        return;
    }
    auto current_work = DiagnosticFrameWork{
        translation_blocks_.load(std::memory_order_relaxed),
        cpu_executions_.load(std::memory_order_relaxed),
        cpu_ticks_.load(std::memory_order_relaxed),
        svc_calls_.load(std::memory_order_relaxed),
        page_misses_.load(std::memory_order_relaxed),
        page_faults_.load(std::memory_order_relaxed),
        draws_.load(std::memory_order_relaxed),
        submits_.load(std::memory_order_relaxed),
        fence_waits_.load(std::memory_order_relaxed),
        fence_wait_nanoseconds_.load(std::memory_order_relaxed),
    };
    for (std::size_t index = 0;
         index < current_work.submit_reasons.size(); ++index) {
        current_work.submit_reasons[index] =
            submit_reasons_[index].load(std::memory_order_relaxed);
    }
    current_work.hle_calls =
        diagnostic_hle_calls_.load(std::memory_order_relaxed);
    current_work.hle_nanoseconds =
        diagnostic_hle_nanoseconds_.load(std::memory_order_relaxed);
    for (std::size_t index = 0;
         index < current_work.graphics_hle_calls.size(); ++index) {
        current_work.graphics_hle_calls[index] =
            diagnostic_graphics_hle_calls_[index].load(
                std::memory_order_relaxed);
        current_work.graphics_hle_nanoseconds[index] =
            diagnostic_graphics_hle_nanoseconds_[index].load(
                std::memory_order_relaxed);
    }
    std::lock_guard window_lock{display_window_mutex_};
    if (!display_window_active_.load(std::memory_order_relaxed))
        return;

    std::uint64_t display_sequence{};
    const auto display = std::find_if(
        diagnostic_display_frames.rbegin(),
        diagnostic_display_frames.rend(),
        [process_id, now](const auto& frame) {
            return frame.owner_process_id == process_id &&
                   frame.vsync_sequence == 0 &&
                   frame.submitted !=
                       std::chrono::steady_clock::time_point{} &&
                   frame.submitted <= now;
        });
    if (display != diagnostic_display_frames.rend()) {
        display_sequence = display->sequence;
        display->vsync_sequence = completed_timeline->sequence;
        display->framebuffer = framebuffer;
    }
    if (diagnostic_vsync_frames.size() <
        maximum_diagnostic_sequence_samples) {
        diagnostic_vsync_frames.push_back(
            DiagnosticVsyncFrame{
                completed_timeline->sequence,
                process_id,
                framebuffer,
                display_sequence,
                completed_timeline->due,
                completed_timeline->callback,
                completed_timeline->swap_end,
                now,
                diagnostic_work_delta(current_work,
                                      diagnostic_previous_frame_work),
            });
    }
    diagnostic_previous_frame_work = current_work;
}

void PerformanceCounters::discard_pending_vsync_callbacks() {
    if (!enabled())
        return;
    std::lock_guard lock{vsync_timeline_mutex_};
    for (auto timeline = vsync_timelines_.begin();
         timeline != vsync_timelines_.end();) {
        std::erase_if(timeline->second, [](const auto& candidate) {
            return candidate.callback !=
                       std::chrono::steady_clock::time_point{} &&
                   candidate.swap_end ==
                       std::chrono::steady_clock::time_point{};
        });
        if (timeline->second.empty()) {
            timeline = vsync_timelines_.erase(timeline);
        } else {
            ++timeline;
        }
    }
}

void PerformanceCounters::record_hle(std::string_view subsystem,
                                     std::uint64_t nanoseconds) {
    if (!enabled())
        return;
    diagnostic_hle_calls_.fetch_add(1, std::memory_order_relaxed);
    diagnostic_hle_nanoseconds_.fetch_add(nanoseconds,
                                          std::memory_order_relaxed);
    auto graphics_kind = PerfDiagnosticGraphicsHleKind::Count;
    if (subsystem == "_mbx2DBlitCopy" ||
        subsystem == "_mbx2DCtxBlitCopy") {
        graphics_kind = PerfDiagnosticGraphicsHleKind::BlitCopy;
    } else if (subsystem == "_mbx2DBlitColor" ||
               subsystem == "_mbx2DCtxBlitColor") {
        graphics_kind = PerfDiagnosticGraphicsHleKind::BlitColor;
    } else if (subsystem == "_mbx3DQuadCopy") {
        graphics_kind = PerfDiagnosticGraphicsHleKind::QuadCopy;
    }
    if (graphics_kind != PerfDiagnosticGraphicsHleKind::Count)
        record_diagnostic_graphics_hle(graphics_kind, nanoseconds);
    std::lock_guard lock{hle_mutex_};
    auto [found, inserted] =
        hle_subsystems_.try_emplace(std::string{subsystem});
    auto& stats = found->second;
    if (inserted)
        stats.subsystem = subsystem;
    ++stats.calls;
    stats.nanoseconds += nanoseconds;
}

void PerformanceCounters::record_diagnostic_graphics_hle(
    PerfDiagnosticGraphicsHleKind kind, std::uint64_t nanoseconds) {
    const auto index = static_cast<std::size_t>(kind);
    if (!enabled() || index >= diagnostic_graphics_hle_calls_.size())
        return;
    diagnostic_graphics_hle_calls_[index].fetch_add(
        1, std::memory_order_relaxed);
    diagnostic_graphics_hle_nanoseconds_[index].fetch_add(
        nanoseconds, std::memory_order_relaxed);
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

void PerformanceCounters::record_global_latency(PerfLatencyKind kind,
                                                std::uint64_t nanoseconds) {
    const auto index = static_cast<std::size_t>(kind);
    if (index >= latencies_.size())
        return;
    auto& histogram = latencies_[index];
    histogram.buckets[latency_bucket(nanoseconds)].fetch_add(
        1, std::memory_order_relaxed);
    histogram.samples.fetch_add(1, std::memory_order_relaxed);
    if (nanoseconds > one_sixtieth_second)
        histogram.over_16_7ms.fetch_add(1, std::memory_order_relaxed);
    if (nanoseconds > twenty_milliseconds)
        histogram.over_20ms.fetch_add(1, std::memory_order_relaxed);
    if (nanoseconds > one_thirtieth_second)
        histogram.over_33_3ms.fetch_add(1, std::memory_order_relaxed);
    if (nanoseconds > fifty_milliseconds)
        histogram.over_50ms.fetch_add(1, std::memory_order_relaxed);
    auto maximum =
        histogram.maximum_nanoseconds.load(std::memory_order_relaxed);
    while (nanoseconds > maximum &&
           !histogram.maximum_nanoseconds.compare_exchange_weak(
               maximum, nanoseconds, std::memory_order_relaxed,
               std::memory_order_relaxed)) {
    }
}

void PerformanceCounters::record_display_window_latency_locked(
    PerfLatencyKind kind, std::uint64_t nanoseconds) {
    const auto index = static_cast<std::size_t>(kind);
    if (!is_display_window_latency(kind) ||
        index >= display_window_latencies_.size()) {
        return;
    }
    auto& histogram = display_window_latencies_[index];
    ++histogram.buckets[latency_bucket(nanoseconds)];
    ++histogram.samples;
    if (nanoseconds > one_sixtieth_second)
        ++histogram.over_16_7ms;
    if (nanoseconds > twenty_milliseconds)
        ++histogram.over_20ms;
    if (nanoseconds > one_thirtieth_second)
        ++histogram.over_33_3ms;
    if (nanoseconds > fifty_milliseconds)
        ++histogram.over_50ms;
    histogram.maximum_nanoseconds =
        std::max(histogram.maximum_nanoseconds, nanoseconds);
    if (const auto sequence = diagnostic_sequence_index(kind);
        sequence &&
        diagnostic_display_sequences[*sequence].size() <
            maximum_diagnostic_sequence_samples) {
        diagnostic_display_sequences[*sequence].push_back(nanoseconds);
    }
}

void PerformanceCounters::record_display_window_latency(
    PerfLatencyKind kind, std::uint64_t nanoseconds) {
    if (!is_display_window_latency(kind) ||
        !display_window_active_.load(std::memory_order_acquire)) {
        return;
    }
    std::lock_guard lock{display_window_mutex_};
    if (!display_window_active_.load(std::memory_order_relaxed))
        return;
    record_display_window_latency_locked(kind, nanoseconds);
}

void PerformanceCounters::record_latency(PerfLatencyKind kind,
                                         std::uint64_t nanoseconds) {
    const auto index = static_cast<std::size_t>(kind);
    if (!enabled() || index >= latencies_.size())
        return;
    record_global_latency(kind, nanoseconds);
    record_display_window_latency(kind, nanoseconds);
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
    result.jit_shared_invalidation_requests =
        jit_shared_invalidation_requests_.load(std::memory_order_relaxed);
    result.translation_blocks =
        translation_blocks_.load(std::memory_order_relaxed);
    result.cpu_executions = cpu_executions_.load(std::memory_order_relaxed);
    result.cpu_ticks = cpu_ticks_.load(std::memory_order_relaxed);
    result.svc_calls = svc_calls_.load(std::memory_order_relaxed);
    result.page_misses = page_misses_.load(std::memory_order_relaxed);
    result.page_faults = page_faults_.load(std::memory_order_relaxed);
    result.draws = draws_.load(std::memory_order_relaxed);
    result.submits = submits_.load(std::memory_order_relaxed);
    for (std::size_t index = 0; index < result.submit_reasons.size();
         ++index) {
        result.submit_reasons[index] =
            submit_reasons_[index].load(std::memory_order_relaxed);
    }
    result.fence_waits = fence_waits_.load(std::memory_order_relaxed);
    result.fence_wait_nanoseconds =
        fence_wait_nanoseconds_.load(std::memory_order_relaxed);
    result.upload_bytes = upload_bytes_.load(std::memory_order_relaxed);
    result.readback_bytes = readback_bytes_.load(std::memory_order_relaxed);
    result.host_fills = host_fills_.load(std::memory_order_relaxed);
    result.host_copies = host_copies_.load(std::memory_order_relaxed);
    result.display_submissions =
        display_submissions_.load(std::memory_order_relaxed);
    result.display_first_nanoseconds =
        display_first_nanoseconds_.load(std::memory_order_relaxed);
    result.display_last_nanoseconds =
        display_last_nanoseconds_.load(std::memory_order_relaxed);
    result.display_mailbox_coalesced =
        display_mailbox_coalesced_.load(std::memory_order_relaxed);
    result.display_vsync_budget_cuts =
        display_vsync_budget_cuts_.load(std::memory_order_relaxed);
    result.display_vsync_budget_saved_ticks =
        display_vsync_budget_saved_ticks_.load(std::memory_order_relaxed);
    result.sdl_idle_waits =
        sdl_idle_waits_.load(std::memory_order_relaxed);
    result.native_present_attempts =
        native_present_attempts_.load(std::memory_order_relaxed);
    result.native_present_mailbox_coalesced =
        native_present_mailbox_coalesced_.load(std::memory_order_relaxed);
    result.native_present_skipped =
        native_present_skipped_.load(std::memory_order_relaxed);
    result.native_present_failures =
        native_present_failures_.load(std::memory_order_relaxed);
    result.native_presents =
        native_presents_.load(std::memory_order_relaxed);
    result.present_first_nanoseconds =
        present_first_nanoseconds_.load(std::memory_order_relaxed);
    result.present_last_nanoseconds =
        present_last_nanoseconds_.load(std::memory_order_relaxed);
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
    for (std::size_t index = 0; index < latencies_.size(); ++index) {
        const auto& histogram = latencies_[index];
        auto& latency = result.latencies[index];
        latency.samples =
            histogram.samples.load(std::memory_order_relaxed);
        latency.maximum_nanoseconds =
            histogram.maximum_nanoseconds.load(std::memory_order_relaxed);
        latency.over_16_7ms =
            histogram.over_16_7ms.load(std::memory_order_relaxed);
        latency.over_20ms =
            histogram.over_20ms.load(std::memory_order_relaxed);
        latency.over_33_3ms =
            histogram.over_33_3ms.load(std::memory_order_relaxed);
        latency.over_50ms =
            histogram.over_50ms.load(std::memory_order_relaxed);
        if (latency.samples == 0)
            continue;
        const auto percentile = [&](std::uint64_t numerator) {
            const auto rank =
                (latency.samples * numerator + 99U) / 100U;
            std::uint64_t cumulative{};
            for (std::size_t bucket = 0;
                 bucket < histogram.buckets.size(); ++bucket) {
                cumulative += histogram.buckets[bucket].load(
                    std::memory_order_relaxed);
                if (cumulative >= rank) {
                    return latency_bucket_upper_bound(
                        bucket, latency.maximum_nanoseconds);
                }
            }
            return latency.maximum_nanoseconds;
        };
        latency.p50_nanoseconds = percentile(50);
        latency.p95_nanoseconds = percentile(95);
        latency.p99_nanoseconds = percentile(99);
    }
    {
        std::lock_guard lock{jit_cache_slots_mutex_};
        result.jit_cache_slots.reserve(jit_cache_slots_.size());
        for (const auto& [key, usage] : jit_cache_slots_) {
            static_cast<void>(key);
            result.jit_cache_slots.push_back(usage);
        }
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

PerformanceLatencyScope::PerformanceLatencyScope(PerfLatencyKind kind)
    : kind_{kind}, enabled_{performance_counters().enabled()},
      started_{enabled_ ? std::chrono::steady_clock::now()
                       : std::chrono::steady_clock::time_point{}} {}

PerformanceLatencyScope::~PerformanceLatencyScope() {
    if (!enabled_)
        return;
    const auto elapsed = std::chrono::steady_clock::now() - started_;
    performance_counters().record_latency(
        kind_, static_cast<std::uint64_t>(
                   std::chrono::duration_cast<std::chrono::nanoseconds>(
                       elapsed)
                       .count()));
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

std::string_view perf_latency_kind_name(PerfLatencyKind kind) {
    switch (kind) {
    case PerfLatencyKind::InputEnqueue: return "input-enqueue";
    case PerfLatencyKind::DisplayPresent: return "display-present";
    case PerfLatencyKind::DisplayMailbox: return "display-mailbox";
    case PerfLatencyKind::NativeMailbox: return "native-mailbox";
    case PerfLatencyKind::Acquire: return "acquire";
    case PerfLatencyKind::QueuePresent: return "queue-present";
    case PerfLatencyKind::PresentReturn: return "present-return";
    case PerfLatencyKind::PresentInterval: return "present-interval";
    case PerfLatencyKind::VsyncDueToCallback:
        return "vsync-due-callback";
    case PerfLatencyKind::VsyncCallbackToSwapEnd:
        return "vsync-callback-swap-end";
    case PerfLatencyKind::VsyncSwapEndToGuestSubmit:
        return "vsync-swap-end-guest-submit";
    case PerfLatencyKind::VsyncDueToGuestSubmit:
        return "vsync-due-guest-submit";
    case PerfLatencyKind::JitColdPath: return "jit-cold-path";
    case PerfLatencyKind::JitBlockCompile: return "jit-block-compile";
    case PerfLatencyKind::RuntimeDestructor: return "runtime-destructor";
    case PerfLatencyKind::GlesTargetRelease: return "gles-target-release";
    case PerfLatencyKind::PosixSpawnTotal: return "posix-spawn-total";
    case PerfLatencyKind::PosixSpawnDecode: return "posix-spawn-decode";
    case PerfLatencyKind::PosixSpawnFork: return "posix-spawn-fork";
    case PerfLatencyKind::PosixSpawnCreate: return "posix-spawn-create";
    case PerfLatencyKind::ProcessFreshMemory: return "process-fresh-memory";
    case PerfLatencyKind::ProcessCloneMemory: return "process-clone-memory";
    case PerfLatencyKind::ProcessCreateCpu: return "process-create-cpu";
    case PerfLatencyKind::ProcessCreateKernel: return "process-create-kernel";
    case PerfLatencyKind::ProcessInheritKernel: return "process-inherit-kernel";
    case PerfLatencyKind::ProcessInheritSpawnKernel:
        return "process-inherit-spawn-kernel";
    case PerfLatencyKind::ProcessConfigureRuntime:
        return "process-configure-runtime";
    case PerfLatencyKind::SpawnMemoryClear: return "spawn-memory-clear";
    case PerfLatencyKind::SpawnImageLoad: return "spawn-image-load";
    case PerfLatencyKind::SpawnResetRuntime: return "spawn-reset-runtime";
    case PerfLatencyKind::Count: break;
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
         << " jit-shared-invalidations="
         << snapshot.jit_shared_invalidation_requests
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
         << " display-submit=" << snapshot.display_submissions
         << " display-coalesced=" << snapshot.display_mailbox_coalesced
         << " display-vsync-budget=" << snapshot.display_vsync_budget_cuts
         << '/' << snapshot.display_vsync_budget_saved_ticks
         << " sdl-idle-waits=" << snapshot.sdl_idle_waits
         << " native-attempt=" << snapshot.native_present_attempts
         << " native-coalesced="
         << snapshot.native_present_mailbox_coalesced
         << " native-skipped=" << snapshot.native_present_skipped
         << " native-failed=" << snapshot.native_present_failures
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
    text << " latency-format=samples/p50/p95/p99/max/>16.7/>20/>33.3/>50"
         << " latency-ns=";
    first = true;
    for (std::size_t index = 0; index < snapshot.latencies.size(); ++index) {
        const auto& latency = snapshot.latencies[index];
        if (!first)
            text << ',';
        first = false;
        text << perf_latency_kind_name(
                    static_cast<PerfLatencyKind>(index))
             << ':' << latency.samples << '/' << latency.p50_nanoseconds
             << '/' << latency.p95_nanoseconds << '/'
             << latency.p99_nanoseconds << '/'
             << latency.maximum_nanoseconds << '/'
             << latency.over_16_7ms << '/' << latency.over_20ms << '/'
             << latency.over_33_3ms << '/' << latency.over_50ms;
    }
    text << " jit-cache-slots=";
    first = true;
    for (const auto& usage : snapshot.jit_cache_slots) {
        if (!first)
            text << ',';
        first = false;
        text << usage.process_id << ':' << usage.slot << ':'
             << usage.current_bytes << '/' << usage.peak_bytes;
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

std::string format_display_performance_summary(
    const PerformanceSnapshot& snapshot, std::string_view label) {
    const auto present_returns =
        snapshot.native_presents + snapshot.cpu_present_fallbacks;
    std::ostringstream text;
    text << std::fixed << std::setprecision(2)
         << "[perf-display] label=" << label
         << " guest-submit=" << snapshot.display_submissions
         << " guest-fps="
         << frames_per_second(snapshot.display_submissions,
                              snapshot.display_first_nanoseconds,
                              snapshot.display_last_nanoseconds)
         << " display-coalesced=" << snapshot.display_mailbox_coalesced
         << " display-vsync-budget=" << snapshot.display_vsync_budget_cuts
         << '/' << snapshot.display_vsync_budget_saved_ticks
         << " native-attempt=" << snapshot.native_present_attempts
         << " native-coalesced="
         << snapshot.native_present_mailbox_coalesced
         << " native-queued=" << snapshot.native_presents
         << " native-skipped=" << snapshot.native_present_skipped
         << " native-failed=" << snapshot.native_present_failures
         << " cpu-present=" << snapshot.cpu_present_fallbacks
         << " present-return=" << present_returns
         << " present-return-fps="
         << frames_per_second(present_returns,
                              snapshot.present_first_nanoseconds,
                              snapshot.present_last_nanoseconds)
         << " latency-format=samples/p50/p95/p99/max/>16.7/>20/>33.3/>50"
         << " latency-ns=";
    constexpr std::array display_latencies{
        PerfLatencyKind::DisplayMailbox,
        PerfLatencyKind::NativeMailbox,
        PerfLatencyKind::Acquire,
        PerfLatencyKind::QueuePresent,
        PerfLatencyKind::PresentReturn,
        PerfLatencyKind::PresentInterval,
        PerfLatencyKind::VsyncDueToCallback,
        PerfLatencyKind::VsyncCallbackToSwapEnd,
        PerfLatencyKind::VsyncSwapEndToGuestSubmit,
        PerfLatencyKind::VsyncDueToGuestSubmit,
    };
    bool first = true;
    for (const auto kind : display_latencies) {
        if (!first)
            text << ',';
        first = false;
        const auto& latency =
            snapshot.latencies[static_cast<std::size_t>(kind)];
        text << perf_latency_kind_name(kind) << ':' << latency.samples << '/'
             << latency.p50_nanoseconds << '/' << latency.p95_nanoseconds
             << '/' << latency.p99_nanoseconds << '/'
             << latency.maximum_nanoseconds << '/'
             << latency.over_16_7ms << '/' << latency.over_20ms << '/'
             << latency.over_33_3ms << '/' << latency.over_50ms;
    }
    text << " diagnostic-sequence-us=";
    for (std::size_t index = 0;
         index < diagnostic_sequence_kinds.size(); ++index) {
        if (index != 0)
            text << ';';
        text << perf_latency_kind_name(diagnostic_sequence_kinds[index])
             << ':';
        const auto& sequence = diagnostic_display_sequences[index];
        for (std::size_t sample = 0; sample < sequence.size(); ++sample) {
            if (sample != 0)
                text << ',';
            text << sequence[sample] / 1'000U;
        }
    }
    text << " diagnostic-window-start-ns="
         << steady_nanoseconds(diagnostic_window_started_at)
         << " diagnostic-timeline-format="
         << "d:frame:pid:vsync:fb:"
            "submit/display-dequeue/native-queue/native-dequeue/"
            "present-return:present-kind|"
         << "v:vsync:pid:fb:frame:"
            "due/callback/swap-end/guest-submit:"
            "blocks/cpu-exec/cpu-ticks/svc/page-miss/page-fault/"
            "draw/submit/fence/fence-ns:"
            "r/submit-reasons:"
            "h/all-calls/all-ns/blit-copy-calls/blit-copy-ns/"
            "blit-color-calls/blit-color-ns/quad-copy-calls/quad-copy-ns"
            "/source-sync-calls/source-sync-ns/"
            "host-copy-calls/host-copy-ns/"
            "host-quad-calls/host-quad-ns"
         << " diagnostic-timeline-us=";
    const auto timeline_empty =
        diagnostic_display_frames.empty() && diagnostic_vsync_frames.empty();
    if (timeline_empty)
        text << "none";
    bool first_timeline = true;
    for (const auto& frame : diagnostic_display_frames) {
        if (!first_timeline)
            text << ',';
        first_timeline = false;
        text << "d:" << frame.sequence << ':' << frame.owner_process_id
             << ':';
        if (frame.vsync_sequence != 0)
            text << frame.vsync_sequence;
        else
            text << '-';
        text << ':';
        if (frame.framebuffer != 0)
            text << frame.framebuffer;
        else
            text << '-';
        text << ':';
        append_diagnostic_timestamp(text, frame.submitted);
        text << '/';
        append_diagnostic_timestamp(text, frame.display_dequeued);
        text << '/';
        append_diagnostic_timestamp(text, frame.native_queued);
        text << '/';
        append_diagnostic_timestamp(text, frame.native_dequeued);
        text << '/';
        append_diagnostic_timestamp(text, frame.present_returned);
        text << ':' << frame.present_kind;
    }
    if (!timeline_empty)
        text << '|';
    bool first_vsync = true;
    for (const auto& frame : diagnostic_vsync_frames) {
        if (!first_vsync)
            text << ',';
        first_vsync = false;
        text << "v:" << frame.sequence << ':' << frame.process_id << ':'
             << frame.framebuffer << ':';
        if (frame.display_sequence != 0)
            text << frame.display_sequence;
        else
            text << '-';
        text << ':';
        append_diagnostic_timestamp(text, frame.due);
        text << '/';
        append_diagnostic_timestamp(text, frame.callback);
        text << '/';
        append_diagnostic_timestamp(text, frame.swap_end);
        text << '/';
        append_diagnostic_timestamp(text, frame.guest_submit);
        text << ':' << frame.work.translation_blocks << '/'
             << frame.work.cpu_executions << '/' << frame.work.cpu_ticks
             << '/' << frame.work.svc_calls << '/'
             << frame.work.page_misses << '/' << frame.work.page_faults
             << '/' << frame.work.draws << '/' << frame.work.submits
             << '/' << frame.work.fence_waits << '/'
             << frame.work.fence_wait_nanoseconds;
        text << ":r";
        for (const auto count : frame.work.submit_reasons)
            text << '/' << count;
        text << ":h/" << frame.work.hle_calls << '/'
             << frame.work.hle_nanoseconds;
        for (std::size_t index = 0;
             index < frame.work.graphics_hle_calls.size(); ++index) {
            text << '/' << frame.work.graphics_hle_calls[index] << '/'
                 << frame.work.graphics_hle_nanoseconds[index];
        }
    }
    text << " diagnostic-work=jit:" << snapshot.jit_instances << '/'
         << snapshot.jit_creation_nanoseconds
         << ",blocks:" << snapshot.translation_blocks
         << ",cpu:" << snapshot.cpu_executions << '/'
         << snapshot.cpu_ticks
         << ",svc:" << snapshot.svc_calls
         << ",draw-submit:" << snapshot.draws << '/'
         << snapshot.submits
         << ",submit-reason[other/mbx-flush/mbx-finish/mbx-surface/"
            "compositor/gles-sync/present/staging/readback/resource/"
            "batch/texture]:";
    for (std::size_t index = 0;
         index < snapshot.submit_reasons.size(); ++index) {
        if (index != 0)
            text << '/';
        text << snapshot.submit_reasons[index];
    }
    text
         << ",fence:" << snapshot.fence_waits << '/'
         << snapshot.fence_wait_nanoseconds
         << ",upload-readback:" << snapshot.upload_bytes << '/'
         << snapshot.readback_bytes
         << ",fill-copy:" << snapshot.host_fills << '/'
         << snapshot.host_copies
         << ",cpu-map:" << snapshot.cpu_map_reads << '/'
         << snapshot.cpu_map_writes
         << " diagnostic-hle=";
    for (std::size_t index = 0;
         index < snapshot.hle_subsystems.size(); ++index) {
        if (index != 0)
            text << ',';
        const auto& hle = snapshot.hle_subsystems[index];
        text << hle.subsystem << ':' << hle.calls << '/'
             << hle.nanoseconds;
    }
    if (snapshot.hle_subsystems.empty())
        text << "none";
    return text.str();
}

} // namespace ilemu
