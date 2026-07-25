#pragma once

#include <array>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <map>
#include <optional>
#include <vector>

namespace ilegacysim {

// Constants and ordering are taken from XNU 792.24.17 osfmk/kern/sched.h
// and sched_prim.c. These defaults serve CPU-only helpers and unit tests. A
// full boot derives its tick rate from the selected device instruction-timing
// model instead of assuming that the CPU clock equals the system bus clock.
namespace xnu792::scheduler {

constexpr std::size_t run_queue_count = 128;
constexpr std::int32_t minimum_priority = 0;
constexpr std::int32_t maximum_priority = 127;
constexpr std::int32_t realtime_base_priority = 96;
constexpr std::int32_t realtime_queue_priority = realtime_base_priority + 1;
constexpr std::int32_t maximum_kernel_priority = 95;
constexpr std::int32_t preempt_priority = maximum_kernel_priority - 2;
constexpr std::int32_t maximum_user_priority = 63;
constexpr std::int32_t default_base_priority = 31;
constexpr std::uint64_t default_guest_ticks_per_second = 400'000'000;
constexpr std::uint64_t default_preemption_rate = 100;
constexpr std::uint64_t scheduler_ticks_per_second = 8;
constexpr std::uint64_t scheduling_usage_decay_ticks = 32;
constexpr std::uint64_t maximum_unsafe_quanta = 800;
constexpr std::uint64_t scheduler_tick_shift = 3;
constexpr std::uint64_t failsafe_release_scheduler_ticks =
    (2 * maximum_unsafe_quanta / default_preemption_rate) *
    (std::uint64_t{1} << scheduler_tick_shift);
constexpr std::uint64_t milliseconds_per_second = 1'000;
constexpr std::uint64_t standard_quantum_ticks =
    default_guest_ticks_per_second / default_preemption_rate;
constexpr std::uint64_t scheduler_tick_interval =
    default_guest_ticks_per_second / scheduler_ticks_per_second;
constexpr std::uint64_t microseconds_per_second = 1'000'000;
constexpr std::uint64_t minimum_realtime_computation_ticks =
    default_guest_ticks_per_second * 50 / microseconds_per_second;
constexpr std::uint64_t maximum_realtime_computation_ticks =
    default_guest_ticks_per_second * 50'000 / microseconds_per_second;

}  // namespace xnu792::scheduler

struct XnuThreadId {
    std::uint32_t process{};
    std::uint32_t thread{};

    auto operator<=>(const XnuThreadId&) const = default;
};

enum class XnuThreadState : std::uint8_t {
    Runnable,
    Running,
    Waiting,
};

enum class XnuSliceCompletion : std::uint8_t {
    Continue,
    Yield,
    Block,
    Terminate,
};

enum class XnuTimeAccounting : std::uint8_t {
    Advance,
    Deferred,
};

enum class XnuPreemption : std::uint8_t {
    None,
    Preempt,
    Urgent,
};

struct XnuScheduledSlice {
    XnuThreadId thread;
    std::size_t processor{};
    std::uint64_t tick_budget{};
};

struct XnuThreadSchedulingInfo {
    XnuThreadState state{XnuThreadState::Waiting};
    std::int32_t base_priority{xnu792::scheduler::default_base_priority};
    std::int32_t scheduled_priority{xnu792::scheduler::default_base_priority};
    std::uint64_t remaining_quantum{xnu792::scheduler::standard_quantum_ticks};
    std::uint64_t scheduling_usage{};
    std::uint64_t cpu_usage{};
    std::uint64_t scheduler_stamp{};
    std::optional<std::size_t> bound_processor;
    std::optional<std::size_t> last_processor;
    std::uint64_t realtime_period{};
    std::uint64_t realtime_computation{};
    std::uint64_t realtime_constraint{};
    std::uint64_t realtime_deadline{};
    std::uint64_t computation_metered{};
    std::uint64_t failsafe_release_tick{};
    std::uint32_t remaining_timeslices{};
    std::optional<std::size_t> timeslice_processor;
    bool timeshare{true};
    bool realtime{};
    bool realtime_preemptible{};
    bool failsafe{};
    bool depressed{};
};

// A deterministic implementation of XNU 792's traditional processor-set run
// queue. It preserves FIFO order at each of the 128 priorities. A thread keeps
// the head position while its first timeslice remains; equal-priority threads
// rotate only when that quantum expires, matching csw_needed().
class XnuScheduler {
public:
    explicit XnuScheduler(
        std::uint64_t quantum_ticks = xnu792::scheduler::standard_quantum_ticks,
        std::uint64_t scheduler_tick_ticks =
            xnu792::scheduler::scheduler_tick_interval,
        std::size_t processor_count = 1);

    bool register_thread(
        XnuThreadId thread,
        std::int32_t base_priority = xnu792::scheduler::default_base_priority,
        bool runnable = true);
    bool remove_thread(XnuThreadId thread);
    std::size_t remove_process(std::uint32_t process);

    bool make_runnable(XnuThreadId thread);
    // Device and IPC completion can race a thread's final blocking SVC. Keep
    // that wake pending until complete_slice observes the Block transition.
    bool wake_thread(XnuThreadId thread);
    bool block(XnuThreadId thread);
    bool suspend_thread(XnuThreadId thread);
    bool resume_thread(XnuThreadId thread);
    bool set_base_priority(XnuThreadId thread, std::int32_t priority);
    bool depress(XnuThreadId thread, std::uint64_t duration_ticks = 0);
    bool bind_thread(
        XnuThreadId thread, std::optional<std::size_t> processor);
    bool set_timeshare(XnuThreadId thread, bool timeshare);
    bool set_realtime(
        XnuThreadId thread, std::uint64_t period_ticks,
        std::uint64_t computation_ticks, std::uint64_t constraint_ticks,
        bool preemptible);

    [[nodiscard]] std::optional<XnuScheduledSlice> choose_next(
        std::optional<XnuThreadId> preferred = std::nullopt);
    [[nodiscard]] std::optional<XnuScheduledSlice> choose_next(
        std::size_t processor,
        std::optional<XnuThreadId> preferred = std::nullopt);
    [[nodiscard]] XnuPreemption preemption_for(
        XnuThreadId running_thread, std::size_t processor) const;
    // XNU's scheduler policy owns the yield decision. Resolve the physical
    // processor from the running thread instead of confusing it with the
    // emulator's per-process CPU-context slot.
    [[nodiscard]] bool should_yield(XnuThreadId running_thread) const;
    bool complete_slice(
        XnuThreadId thread, std::uint64_t consumed_ticks,
        XnuSliceCompletion completion,
        XnuTimeAccounting time_accounting = XnuTimeAccounting::Advance);
    void advance_time(std::uint64_t elapsed_ticks);

    [[nodiscard]] bool contains(XnuThreadId thread) const;
    [[nodiscard]] std::optional<XnuThreadSchedulingInfo> info(
        XnuThreadId thread) const;
    [[nodiscard]] std::size_t thread_count() const { return threads_.size(); }
    [[nodiscard]] std::size_t processor_count() const {
        return processor_run_queues_.size();
    }
    [[nodiscard]] std::size_t runnable_count() const { return runnable_count_; }
    [[nodiscard]] std::size_t waiting_count() const;
    [[nodiscard]] std::int32_t highest_runnable_priority() const;
    [[nodiscard]] std::uint64_t scheduler_tick() const { return scheduler_tick_; }

private:
    struct RunQueue {
        std::array<std::deque<XnuThreadId>, xnu792::scheduler::run_queue_count>
            queues;
        std::array<std::uint32_t,
                   xnu792::scheduler::run_queue_count / 32>
            bitmap{};
        std::int32_t high_queue{-1};
        std::size_t count{};
    };

    struct ThreadRecord {
        XnuThreadSchedulingInfo info;
        bool queued{};
        std::optional<std::uint32_t> priority_usage_shift;
        std::optional<std::size_t> queued_processor;
        std::optional<std::uint64_t> depression_deadline;
        std::uint32_t suspend_count{};
        bool resume_runnable{};
        bool wake_pending{};
        std::int32_t failsafe_saved_base_priority{};
        bool failsafe_saved_timeshare{};
        bool failsafe_saved_realtime{};
    };

    enum class QueuePosition { Front, Back };

    static std::int32_t clamp_priority(std::int32_t priority);
    static std::uint32_t priority_usage_shift(
        std::uint64_t scheduler_tick_ticks);
    void enqueue(XnuThreadId thread, QueuePosition position);
    void remove_from_queue(XnuThreadId thread, ThreadRecord& record);
    static void refresh_high_queue(RunQueue& run_queue);
    [[nodiscard]] static XnuThreadId pop_highest(RunQueue& run_queue);
    void advance_scheduler_time(std::uint64_t consumed_ticks);
    void age_priorities(std::uint64_t elapsed_ticks);
    void expire_depressions();
    void restore_depression(XnuThreadId thread, ThreadRecord& record);
    void recompute_priority(XnuThreadId thread, ThreadRecord& record);
    [[nodiscard]] std::uint64_t quantum_for(const ThreadRecord& record) const;
    [[nodiscard]] std::optional<std::uint32_t>
    processor_set_priority_shift() const;
    void apply_failsafe(XnuThreadId thread, ThreadRecord& record);
    void release_failsafe(XnuThreadId thread, ThreadRecord& record);
    [[nodiscard]] std::uint32_t timeshare_quanta() const;

    RunQueue processor_set_run_queue_;
    std::vector<RunQueue> processor_run_queues_;
    std::map<XnuThreadId, ThreadRecord> threads_;
    std::size_t runnable_count_{};
    std::uint64_t quantum_ticks_{};
    std::uint64_t scheduler_tick_ticks_{};
    std::uint32_t priority_usage_shift_{};
    std::uint64_t elapsed_since_scheduler_tick_{};
    std::uint64_t elapsed_ticks_{};
    std::uint64_t scheduler_tick_{};
};

}  // namespace ilegacysim
