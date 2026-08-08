#include "ilemu/xnu_scheduler.hpp"

#include <algorithm>
#include <bit>
#include <limits>
#include <stdexcept>

namespace ilemu {

namespace {

std::uint64_t saturating_add(std::uint64_t left, std::uint64_t right) {
    if (right > std::numeric_limits<std::uint64_t>::max() - left) {
        return std::numeric_limits<std::uint64_t>::max();
    }
    return left + right;
}

std::uint64_t decay_once(std::uint64_t usage) {
    // sched_decay_shifts[1] in XNU 792 priority.c: 1/2 + 1/8 = 5/8.
    return (usage >> 1U) + (usage >> 3U);
}

}  // namespace

XnuScheduler::XnuScheduler(
    std::uint64_t quantum_ticks, std::uint64_t scheduler_tick_ticks,
    std::size_t processor_count)
    : processor_run_queues_(processor_count),
      quantum_ticks_{quantum_ticks}, scheduler_tick_ticks_{scheduler_tick_ticks} {
    if (quantum_ticks_ == 0 || scheduler_tick_ticks_ == 0 ||
        processor_run_queues_.empty()) {
        throw std::invalid_argument{"XNU scheduler intervals must be non-zero"};
    }
    priority_usage_shift_ = priority_usage_shift(scheduler_tick_ticks_);
}

bool XnuScheduler::register_thread(
    XnuThreadId thread, std::int32_t base_priority, bool runnable) {
    const auto [iterator, inserted] = threads_.try_emplace(thread);
    if (!inserted) return false;

    auto &record = iterator->second;
    record.info.base_priority = clamp_priority(base_priority);
    record.info.scheduled_priority = record.info.base_priority;
    record.info.remaining_quantum = quantum_ticks_;
    record.info.scheduler_stamp = scheduler_tick_;
    record.info.state = runnable ? XnuThreadState::Runnable
                                 : XnuThreadState::Waiting;
    process_threads_[thread.process].insert(thread);
    if (runnable) {
        ++active_timeshare_count_;
        enqueue(thread, QueuePosition::Back);
    } else {
        ++waiting_count_;
    }
    return true;
}

bool XnuScheduler::remove_thread(XnuThreadId thread) {
    const auto iterator = threads_.find(thread);
    if (iterator == threads_.end()) return false;
    if (iterator->second.info.state == XnuThreadState::Waiting) {
        --waiting_count_;
    } else if (iterator->second.info.timeshare) {
        --active_timeshare_count_;
    }
    remove_from_queue(thread, iterator->second);
    unindex_thread(thread);
    threads_.erase(iterator);
    return true;
}

std::size_t XnuScheduler::remove_process(std::uint32_t process) {
    const auto process_iterator = process_threads_.find(process);
    if (process_iterator == process_threads_.end()) return 0;
    const std::vector<XnuThreadId> process_threads{
        process_iterator->second.begin(), process_iterator->second.end()};
    for (const auto thread : process_threads) {
        static_cast<void>(remove_thread(thread));
    }
    return process_threads.size();
}

bool XnuScheduler::make_runnable(XnuThreadId thread) {
    const auto iterator = threads_.find(thread);
    if (iterator == threads_.end()) return false;
    auto& record = iterator->second;
    if (record.suspend_count != 0) {
        record.resume_runnable = true;
        return true;
    }
    if (record.info.state == XnuThreadState::Running || record.queued) return true;
    if (record.info.state == XnuThreadState::Waiting) {
        --waiting_count_;
        if (record.info.timeshare) {
            ++active_timeshare_count_;
        }
    }
    record.info.state = XnuThreadState::Runnable;
    record.info.remaining_quantum = quantum_for(record);
    record.info.computation_metered = 0;
    if (record.info.realtime) {
        record.info.realtime_deadline = saturating_add(
            elapsed_ticks_, record.info.realtime_constraint);
    }
    enqueue(thread, QueuePosition::Back);
    return true;
}

bool XnuScheduler::wake_thread(XnuThreadId thread) {
    const auto iterator = threads_.find(thread);
    if (iterator == threads_.end()) return false;
    if (iterator->second.info.state == XnuThreadState::Running) {
        iterator->second.wake_pending = true;
        return true;
    }
    return make_runnable(thread);
}

bool XnuScheduler::block(XnuThreadId thread) {
    const auto iterator = threads_.find(thread);
    if (iterator == threads_.end()) return false;
    auto& record = iterator->second;
    const auto was_waiting = record.info.state == XnuThreadState::Waiting;
    remove_from_queue(thread, record);
    record.info.state = XnuThreadState::Waiting;
    if (!was_waiting) {
        ++waiting_count_;
        if (record.info.timeshare) {
            --active_timeshare_count_;
        }
    }
    record.info.remaining_quantum = quantum_for(record);
    record.info.computation_metered = 0;
    if (record.suspend_count != 0) record.resume_runnable = false;
    return true;
}

bool XnuScheduler::suspend_thread(XnuThreadId thread) {
    const auto iterator = threads_.find(thread);
    if (iterator == threads_.end() ||
        iterator->second.suspend_count ==
            std::numeric_limits<std::uint32_t>::max()) {
        return false;
    }

    auto& record = iterator->second;
    if (record.suspend_count++ != 0) return true;
    record.resume_runnable =
        record.info.state == XnuThreadState::Runnable ||
        record.info.state == XnuThreadState::Running;
    if (record.info.state != XnuThreadState::Waiting) {
        remove_from_queue(thread, record);
        record.info.state = XnuThreadState::Waiting;
        ++waiting_count_;
        if (record.info.timeshare) {
            --active_timeshare_count_;
        }
    }
    return true;
}

bool XnuScheduler::resume_thread(XnuThreadId thread) {
    const auto iterator = threads_.find(thread);
    if (iterator == threads_.end() || iterator->second.suspend_count == 0)
        return false;

    auto& record = iterator->second;
    --record.suspend_count;
    if (record.suspend_count != 0 || !record.resume_runnable) return true;
    record.resume_runnable = false;
    --waiting_count_;
    record.info.state = XnuThreadState::Runnable;
    if (record.info.timeshare) {
        ++active_timeshare_count_;
    }
    record.info.remaining_quantum = quantum_for(record);
    record.info.computation_metered = 0;
    enqueue(thread, QueuePosition::Back);
    return true;
}

bool XnuScheduler::set_base_priority(XnuThreadId thread, std::int32_t priority) {
    const auto iterator = threads_.find(thread);
    if (iterator == threads_.end()) return false;
    const auto clamped = clamp_priority(priority);
    if (iterator->second.info.base_priority == clamped) return true;
    iterator->second.info.base_priority = clamped;
    recompute_priority(thread, iterator->second);
    return true;
}

bool XnuScheduler::depress(XnuThreadId thread, std::uint64_t duration_ticks) {
    const auto iterator = threads_.find(thread);
    if (iterator == threads_.end()) return false;
    auto& record = iterator->second;
    if (record.info.depressed) return true;
    const auto was_queued = record.queued;
    if (was_queued) remove_from_queue(thread, record);
    record.info.depressed = true;
    record.info.scheduled_priority = xnu792::scheduler::minimum_priority;
    if (duration_ticks != 0) {
        record.depression_deadline = saturating_add(elapsed_ticks_, duration_ticks);
        index_depression(thread, record);
    } else {
        record.depression_deadline.reset();
    }
    if (was_queued) enqueue(thread, QueuePosition::Back);
    return true;
}

bool XnuScheduler::bind_thread(
    XnuThreadId thread, std::optional<std::size_t> processor) {
    if (processor && *processor >= processor_run_queues_.size()) return false;
    const auto iterator = threads_.find(thread);
    if (iterator == threads_.end()) return false;
    auto& record = iterator->second;
    if (record.info.bound_processor == processor) return true;
    const auto was_queued = record.queued;
    if (was_queued) remove_from_queue(thread, record);
    record.info.bound_processor = processor;
    if (was_queued) enqueue(thread, QueuePosition::Back);
    return true;
}

bool XnuScheduler::set_timeshare(XnuThreadId thread, bool timeshare) {
    const auto iterator = threads_.find(thread);
    if (iterator == threads_.end()) return false;
    auto& record = iterator->second;
    if (!record.info.realtime && record.info.timeshare == timeshare) return true;
    const auto was_timeshare = record.info.timeshare;
    record.info.realtime = false;
    record.info.timeshare = timeshare;
    if (record.info.state != XnuThreadState::Waiting &&
        was_timeshare != timeshare) {
        if (timeshare) {
            ++active_timeshare_count_;
        } else {
            --active_timeshare_count_;
        }
    }
    record.info.remaining_quantum = quantum_ticks_;
    recompute_priority(thread, record);
    return true;
}

bool XnuScheduler::set_realtime(
    XnuThreadId thread, std::uint64_t period_ticks,
    std::uint64_t computation_ticks, std::uint64_t constraint_ticks,
    bool preemptible) {
    if (constraint_ticks < computation_ticks ||
        computation_ticks <
            xnu792::scheduler::minimum_realtime_computation_ticks ||
        computation_ticks >
            xnu792::scheduler::maximum_realtime_computation_ticks) {
        return false;
    }
    const auto iterator = threads_.find(thread);
    if (iterator == threads_.end()) return false;
    auto& record = iterator->second;
    const auto was_timeshare = record.info.timeshare;
    record.info.timeshare = false;
    record.info.realtime = true;
    if (record.info.state != XnuThreadState::Waiting && was_timeshare) {
        --active_timeshare_count_;
    }
    record.info.realtime_preemptible = preemptible;
    record.info.realtime_period = period_ticks;
    record.info.realtime_computation = computation_ticks;
    record.info.realtime_constraint = constraint_ticks;
    record.info.realtime_deadline =
        saturating_add(elapsed_ticks_, constraint_ticks);
    record.info.remaining_quantum = computation_ticks;
    recompute_priority(thread, record);
    return true;
}

std::optional<XnuScheduledSlice> XnuScheduler::choose_next(
    std::optional<XnuThreadId> preferred) {
    return choose_next(0, preferred);
}

std::optional<XnuScheduledSlice> XnuScheduler::choose_next(
    std::size_t processor, std::optional<XnuThreadId> preferred) {
    if (processor >= processor_run_queues_.size()) return std::nullopt;
    if (preferred) {
        const auto iterator = threads_.find(*preferred);
        if (iterator == threads_.end() || !iterator->second.queued ||
            (iterator->second.info.bound_processor &&
             *iterator->second.info.bound_processor != processor)) {
            return std::nullopt;
        }
        auto& record = iterator->second;
        remove_from_queue(*preferred, record);
        if (record.info.depressed) restore_depression(*preferred, record);
        record.info.state = XnuThreadState::Running;
        record.info.last_processor = processor;
        if (record.info.timeshare &&
            (record.info.remaining_timeslices == 0 ||
             record.info.timeslice_processor != processor)) {
            record.info.remaining_timeslices = timeshare_quanta();
            record.info.timeslice_processor = processor;
        }
        record.priority_usage_shift = processor_set_priority_shift();
        return XnuScheduledSlice{
            *preferred, processor, record.info.remaining_quantum};
    }

    auto* selected_queue = selected_run_queue(processor);
    if (selected_queue == nullptr) return std::nullopt;

    const auto thread = pop_highest(*selected_queue);
    auto& record = threads_.at(thread);
    record.queued = false;
    record.queued_processor.reset();
    --runnable_count_;
    if (record.info.depressed) restore_depression(thread, record);
    record.info.state = XnuThreadState::Running;
    record.info.last_processor = processor;
    if (record.info.timeshare &&
        (record.info.remaining_timeslices == 0 ||
         record.info.timeslice_processor != processor)) {
        record.info.remaining_timeslices = timeshare_quanta();
        record.info.timeslice_processor = processor;
    }
    record.priority_usage_shift = processor_set_priority_shift();
    return XnuScheduledSlice{
        thread, processor, record.info.remaining_quantum};
}

XnuPreemption XnuScheduler::preemption_for(
    XnuThreadId running_thread, std::size_t processor) const {
    const auto current = threads_.find(running_thread);
    if (processor >= processor_run_queues_.size() ||
        current == threads_.end() ||
        current->second.info.state != XnuThreadState::Running) {
        return XnuPreemption::None;
    }

    const auto candidate_thread = peek_next_for_processor(processor);
    if (!candidate_thread) return XnuPreemption::None;

    const auto candidate = threads_.find(*candidate_thread);
    if (candidate == threads_.end()) return XnuPreemption::None;
    const auto candidate_priority = candidate->second.info.scheduled_priority;
    const auto current_priority = current->second.info.scheduled_priority;
    const auto first_timeslice =
        current->second.info.remaining_quantum != 0;
    bool preempt = first_timeslice
        ? candidate_priority > current_priority
        : candidate_priority >= current_priority;
    if (!preempt &&
        candidate_priority >= xnu792::scheduler::realtime_queue_priority &&
        current->second.info.realtime && candidate->second.info.realtime &&
        candidate->second.info.realtime_deadline <
            current->second.info.realtime_deadline) {
        preempt = true;
    }
    if (!preempt) return XnuPreemption::None;
    return !candidate->second.info.timeshare &&
                   candidate_priority >= xnu792::scheduler::preempt_priority
        ? XnuPreemption::Urgent
        : XnuPreemption::Preempt;
}

bool XnuScheduler::should_yield(XnuThreadId running_thread) const {
    const auto current = threads_.find(running_thread);
    if (current == threads_.end() ||
        current->second.info.state != XnuThreadState::Running ||
        !current->second.info.last_processor) {
        return false;
    }
    const auto processor = *current->second.info.last_processor;
    if (processor >= processor_run_queues_.size()) return false;
    return processor_run_queues_[processor].count != 0 ||
           processor_set_run_queue_.count != 0;
}

bool XnuScheduler::complete_slice(
    XnuThreadId thread, std::uint64_t consumed_ticks,
    XnuSliceCompletion completion, XnuTimeAccounting time_accounting) {
    const auto iterator = threads_.find(thread);
    if (iterator == threads_.end() ||
        iterator->second.info.state != XnuThreadState::Running) {
        return false;
    }

    auto& record = iterator->second;
    if (record.priority_usage_shift && record.info.timeshare) {
        record.info.scheduling_usage = saturating_add(
            record.info.scheduling_usage, consumed_ticks);
    }
    record.info.cpu_usage = saturating_add(record.info.cpu_usage, consumed_ticks);
    record.info.remaining_quantum -=
        std::min(record.info.remaining_quantum, consumed_ticks);
    if (!record.info.timeshare) {
        record.info.computation_metered = saturating_add(
            record.info.computation_metered, consumed_ticks);
    }
    if (record.info.remaining_quantum == 0 && !record.info.timeshare &&
        record.info.computation_metered >
            xnu792::scheduler::maximum_unsafe_quanta * quantum_ticks_) {
        apply_failsafe(thread, record);
    }
    if (time_accounting == XnuTimeAccounting::Advance) {
        advance_scheduler_time(consumed_ticks);
    }

    if (completion == XnuSliceCompletion::Terminate) {
        if (record.info.timeshare) {
            --active_timeshare_count_;
        }
        unindex_thread(thread);
        threads_.erase(iterator);
        return true;
    }
    if (completion == XnuSliceCompletion::Block) {
        if (record.wake_pending) {
            record.wake_pending = false;
            record.info.state = XnuThreadState::Runnable;
            record.info.remaining_quantum = quantum_for(record);
            record.info.remaining_timeslices = 0;
            record.info.timeslice_processor.reset();
            enqueue(thread, QueuePosition::Back);
            return true;
        }
        record.info.state = XnuThreadState::Waiting;
        ++waiting_count_;
        if (record.info.timeshare) {
            --active_timeshare_count_;
        }
        record.info.remaining_quantum = quantum_for(record);
        record.info.remaining_timeslices = 0;
        record.info.timeslice_processor.reset();
        return true;
    }

    record.wake_pending = false;

    const auto quantum_expired = record.info.remaining_quantum == 0;
    if (completion == XnuSliceCompletion::Yield || quantum_expired) {
        if (completion == XnuSliceCompletion::Yield) {
            record.info.computation_metered = 0;
        }
        record.info.remaining_quantum = quantum_for(record);
        recompute_priority(thread, record);
        record.info.state = XnuThreadState::Runnable;
        if (completion != XnuSliceCompletion::Yield && record.info.timeshare &&
            record.info.remaining_timeslices > 1) {
            --record.info.remaining_timeslices;
            enqueue(thread, QueuePosition::Front);
        } else {
            record.info.remaining_timeslices = 0;
            record.info.timeslice_processor.reset();
            enqueue(thread, QueuePosition::Back);
        }
    } else {
        record.info.state = XnuThreadState::Runnable;
        enqueue(thread, QueuePosition::Front);
    }
    return true;
}

void XnuScheduler::advance_time(std::uint64_t elapsed_ticks) {
    advance_scheduler_time(elapsed_ticks);
}

bool XnuScheduler::contains(XnuThreadId thread) const {
    return threads_.contains(thread);
}

std::optional<XnuThreadSchedulingInfo> XnuScheduler::info(
    XnuThreadId thread) const {
    const auto iterator = threads_.find(thread);
    if (iterator == threads_.end()) return std::nullopt;
    return iterator->second.info;
}

std::size_t XnuScheduler::waiting_count() const {
    return waiting_count_;
}

std::int32_t XnuScheduler::highest_runnable_priority() const {
    auto priority = processor_set_run_queue_.high_queue;
    for (const auto& run_queue : processor_run_queues_) {
        priority = std::max(priority, run_queue.high_queue);
    }
    return priority;
}

std::int32_t XnuScheduler::clamp_priority(std::int32_t priority) {
    return std::clamp(
        priority, xnu792::scheduler::minimum_priority,
        xnu792::scheduler::maximum_priority);
}

std::uint32_t XnuScheduler::priority_usage_shift(
    std::uint64_t scheduler_tick_ticks) {
    // sched_timebase_init() scales the 8 Hz interval by 5/3, then finds the
    // first right shift that brings it down to BASEPRI_DEFAULT.
    auto scaled_interval = (scheduler_tick_ticks / 3U) * 5U +
                           ((scheduler_tick_ticks % 3U) * 5U) / 3U;
    std::uint32_t shift = 0;
    while (scaled_interval > static_cast<std::uint64_t>(
                                  xnu792::scheduler::default_base_priority)) {
        scaled_interval >>= 1U;
        ++shift;
    }
    return shift;
}

void XnuScheduler::enqueue(XnuThreadId thread, QueuePosition position) {
    auto& record = threads_.at(thread);
    if (record.queued) return;
    const auto priority = record.info.scheduled_priority;
    auto& run_queue = record.info.bound_processor
        ? processor_run_queues_.at(*record.info.bound_processor)
        : processor_set_run_queue_;
    auto& queue = run_queue.queues[static_cast<std::size_t>(priority)];
    if (record.info.realtime &&
        priority >= xnu792::scheduler::realtime_queue_priority) {
        const auto realtime_key =
            RealtimeQueueKey{record.info.realtime_deadline, thread};
        run_queue.realtime_order.insert(realtime_key);
        record.realtime_queue_key = realtime_key;
        record.queue_position = queue.insert(queue.end(), thread);
    } else if (position == QueuePosition::Front) {
        record.queue_position = queue.insert(queue.begin(), thread);
    } else {
        record.queue_position = queue.insert(queue.end(), thread);
    }
    record.queued = true;
    record.queued_priority = priority;
    record.queued_processor = record.info.bound_processor;
    ++runnable_count_;
    ++run_queue.count;
    run_queue.bitmap[static_cast<std::size_t>(priority) / 32U] |=
        std::uint32_t{1} << (static_cast<std::uint32_t>(priority) % 32U);
    run_queue.high_queue = std::max(run_queue.high_queue, priority);
}

XnuScheduler::RunQueue* XnuScheduler::selected_run_queue(
    std::size_t processor) {
    if (processor >= processor_run_queues_.size()) return nullptr;
    auto& local_run_queue = processor_run_queues_[processor];
    if (local_run_queue.count != 0 &&
        local_run_queue.high_queue >= processor_set_run_queue_.high_queue) {
        return &local_run_queue;
    }
    if (processor_set_run_queue_.count != 0) return &processor_set_run_queue_;
    return nullptr;
}

const XnuScheduler::RunQueue* XnuScheduler::selected_run_queue(
    std::size_t processor) const {
    if (processor >= processor_run_queues_.size()) return nullptr;
    const auto& local_run_queue = processor_run_queues_[processor];
    if (local_run_queue.count != 0 &&
        local_run_queue.high_queue >= processor_set_run_queue_.high_queue) {
        return &local_run_queue;
    }
    if (processor_set_run_queue_.count != 0) return &processor_set_run_queue_;
    return nullptr;
}

XnuThreadId XnuScheduler::peek_highest(const RunQueue& run_queue) const {
    if (run_queue.count == 0 ||
        run_queue.high_queue < xnu792::scheduler::minimum_priority) {
        throw std::logic_error{"cannot peek an empty XNU run queue"};
    }
    const auto priority = run_queue.high_queue;
    if (priority >= xnu792::scheduler::realtime_queue_priority) {
        if (run_queue.realtime_order.empty()) {
            throw std::logic_error{"XNU realtime queue index is inconsistent"};
        }
        const auto thread = run_queue.realtime_order.begin()->second;
        const auto iterator = threads_.find(thread);
        if (iterator == threads_.end() || !iterator->second.queued ||
            iterator->second.queued_priority != priority) {
            throw std::logic_error{"XNU realtime queue index is inconsistent"};
        }
        return thread;
    }
    const auto& queue = run_queue.queues[static_cast<std::size_t>(priority)];
    if (queue.empty()) {
        throw std::logic_error{"XNU run queue bitmap is inconsistent"};
    }
    return queue.front();
}

std::optional<XnuThreadId> XnuScheduler::peek_next_for_processor(
    std::size_t processor) const {
    const auto* selected_queue = selected_run_queue(processor);
    if (selected_queue == nullptr) return std::nullopt;
    return peek_highest(*selected_queue);
}

void XnuScheduler::index_depression(
    XnuThreadId thread, const ThreadRecord& record) {
    if (record.depression_deadline) {
        depression_order_.emplace(*record.depression_deadline, thread);
    }
}

void XnuScheduler::unindex_depression(
    XnuThreadId thread, const ThreadRecord& record) {
    if (record.depression_deadline) {
        depression_order_.erase({*record.depression_deadline, thread});
    }
}

void XnuScheduler::index_failsafe(
    XnuThreadId thread, const ThreadRecord& record) {
    if (record.info.failsafe) {
        failsafe_order_.emplace(record.info.failsafe_release_tick, thread);
    }
}

void XnuScheduler::unindex_failsafe(
    XnuThreadId thread, const ThreadRecord& record) {
    if (record.info.failsafe) {
        failsafe_order_.erase({record.info.failsafe_release_tick, thread});
    }
}

void XnuScheduler::remove_from_queue(XnuThreadId, ThreadRecord& record) {
    if (!record.queued) return;
    const auto priority = record.queued_priority;
    auto& run_queue = record.queued_processor
        ? processor_run_queues_.at(*record.queued_processor)
        : processor_set_run_queue_;
    auto& queue = run_queue.queues[static_cast<std::size_t>(priority)];
    if (!record.queue_position) {
        throw std::logic_error{"XNU scheduler queue membership is inconsistent"};
    }
    queue.erase(*record.queue_position);
    record.queue_position.reset();
    if (record.realtime_queue_key) {
        run_queue.realtime_order.erase(*record.realtime_queue_key);
        record.realtime_queue_key.reset();
    }
    record.queued = false;
    record.queued_processor.reset();
    --runnable_count_;
    --run_queue.count;
    if (queue.empty()) {
        run_queue.bitmap[static_cast<std::size_t>(priority) / 32U] &=
            ~(std::uint32_t{1} << (static_cast<std::uint32_t>(priority) % 32U));
        if (priority == run_queue.high_queue) refresh_high_queue(run_queue);
    }
}

void XnuScheduler::refresh_high_queue(RunQueue& run_queue) {
    run_queue.high_queue = -1;
    for (std::size_t word_index = run_queue.bitmap.size(); word_index-- > 0;) {
        const auto word = run_queue.bitmap[word_index];
        if (word != 0) {
            const auto highest_bit =
                static_cast<std::size_t>(
                    31U - static_cast<unsigned>(std::countl_zero(word)));
            run_queue.high_queue = static_cast<std::int32_t>(
                word_index * 32U + highest_bit);
            return;
        }
    }
}

XnuThreadId XnuScheduler::pop_highest(RunQueue& run_queue) {
    if (run_queue.count == 0 ||
        run_queue.high_queue < xnu792::scheduler::minimum_priority) {
        throw std::logic_error{"cannot pop an empty XNU run queue"};
    }
    const auto priority = run_queue.high_queue;
    auto& queue = run_queue.queues[static_cast<std::size_t>(priority)];
    const auto thread = peek_highest(run_queue);
    if (priority >= xnu792::scheduler::realtime_queue_priority &&
        !run_queue.realtime_order.empty()) {
        const auto realtime_iterator = run_queue.realtime_order.begin();
        auto &record = threads_.at(thread);
        if (!record.queue_position) {
            throw std::logic_error{
                "XNU realtime queue index is inconsistent"};
        }
        queue.erase(*record.queue_position);
        record.queue_position.reset();
        record.realtime_queue_key.reset();
        run_queue.realtime_order.erase(realtime_iterator);
    } else {
        auto &record = threads_.at(thread);
        record.queue_position.reset();
        queue.pop_front();
    }
    --run_queue.count;
    if (queue.empty()) {
        run_queue.bitmap[static_cast<std::size_t>(priority) / 32U] &=
            ~(std::uint32_t{1} << (static_cast<std::uint32_t>(priority) % 32U));
        refresh_high_queue(run_queue);
    }
    return thread;
}

void XnuScheduler::unindex_thread(XnuThreadId thread) {
    const auto thread_iterator = threads_.find(thread);
    if (thread_iterator != threads_.end()) {
        unindex_depression(thread, thread_iterator->second);
        unindex_failsafe(thread, thread_iterator->second);
    }
    const auto process_iterator = process_threads_.find(thread.process);
    if (process_iterator == process_threads_.end()) return;
    process_iterator->second.erase(thread);
    if (process_iterator->second.empty()) {
        process_threads_.erase(process_iterator);
    }
}

void XnuScheduler::advance_scheduler_time(std::uint64_t consumed_ticks) {
    elapsed_ticks_ = saturating_add(elapsed_ticks_, consumed_ticks);
    expire_depressions();
    elapsed_since_scheduler_tick_ = saturating_add(
        elapsed_since_scheduler_tick_, consumed_ticks);
    const auto elapsed_ticks = elapsed_since_scheduler_tick_ / scheduler_tick_ticks_;
    elapsed_since_scheduler_tick_ %= scheduler_tick_ticks_;
    if (elapsed_ticks == 0) return;
    scheduler_tick_ = saturating_add(scheduler_tick_, elapsed_ticks);
    age_priorities(elapsed_ticks);
}

void XnuScheduler::expire_depressions() {
    while (!depression_order_.empty() &&
           depression_order_.begin()->first <= elapsed_ticks_) {
        const auto [deadline, thread] = *depression_order_.begin();
        const auto iterator = threads_.find(thread);
        if (iterator == threads_.end() ||
            !iterator->second.info.depressed ||
            !iterator->second.depression_deadline ||
            *iterator->second.depression_deadline != deadline) {
            depression_order_.erase(depression_order_.begin());
            continue;
        }
        restore_depression(thread, iterator->second);
    }
}

void XnuScheduler::restore_depression(
    XnuThreadId thread, ThreadRecord& record) {
    if (!record.info.depressed) return;
    unindex_depression(thread, record);
    record.info.depressed = false;
    record.depression_deadline.reset();
    recompute_priority(thread, record);
}

void XnuScheduler::age_priorities(std::uint64_t elapsed_ticks) {
    const auto processor_set_shift = processor_set_priority_shift();
    while (!failsafe_order_.empty() &&
           failsafe_order_.begin()->first <= scheduler_tick_) {
        const auto [release_tick, thread] = *failsafe_order_.begin();
        const auto iterator = threads_.find(thread);
        if (iterator == threads_.end() || !iterator->second.info.failsafe ||
            iterator->second.info.failsafe_release_tick != release_tick) {
            failsafe_order_.erase(failsafe_order_.begin());
            continue;
        }
        release_failsafe(thread, iterator->second);
    }
    for (auto& [thread, record] : threads_) {
        record.priority_usage_shift = processor_set_shift;
        if (elapsed_ticks >= xnu792::scheduler::scheduling_usage_decay_ticks) {
            record.info.scheduling_usage = 0;
            record.info.cpu_usage = 0;
        } else {
            for (std::uint64_t tick = 0; tick < elapsed_ticks; ++tick) {
                record.info.scheduling_usage =
                    decay_once(record.info.scheduling_usage);
                record.info.cpu_usage = decay_once(record.info.cpu_usage);
            }
        }
        record.info.scheduler_stamp = scheduler_tick_;
        recompute_priority(thread, record);
    }
}

void XnuScheduler::recompute_priority(
    XnuThreadId thread, ThreadRecord& record) {
    if (record.info.depressed) {
        return;
    }
    const auto penalty = static_cast<std::int32_t>(std::min<std::uint64_t>(
        record.priority_usage_shift
            ? record.info.scheduling_usage >> *record.priority_usage_shift
            : 0,
        static_cast<std::uint64_t>(xnu792::scheduler::maximum_priority)));
    const auto priority = record.info.realtime
        ? xnu792::scheduler::realtime_queue_priority
        : record.info.timeshare
              ? std::clamp(record.info.base_priority - penalty,
                           xnu792::scheduler::minimum_priority,
                           xnu792::scheduler::maximum_kernel_priority)
              : record.info.base_priority;
    if (priority == record.info.scheduled_priority) return;
    const auto was_queued = record.queued;
    if (was_queued) remove_from_queue(thread, record);
    record.info.scheduled_priority = priority;
    if (was_queued) enqueue(thread, QueuePosition::Back);
}

std::uint64_t XnuScheduler::quantum_for(const ThreadRecord& record) const {
    return record.info.realtime ? record.info.realtime_computation
                                : quantum_ticks_;
}

std::optional<std::uint32_t>
XnuScheduler::processor_set_priority_shift() const {
    const auto shared_threads = active_timeshare_count_;
    const auto processors = processor_run_queues_.size();
    if (shared_threads <= processors) return std::nullopt;

    auto load = processors > 1 ? shared_threads / processors : shared_threads;
    load = std::min(load, xnu792::scheduler::run_queue_count - 1U);
    std::uint32_t load_shift = 0;
    while (load > 1U) {
        load >>= 1U;
        ++load_shift;
    }
    return load_shift < priority_usage_shift_
        ? std::optional<std::uint32_t>{priority_usage_shift_ - load_shift}
        : std::optional<std::uint32_t>{0};
}

void XnuScheduler::apply_failsafe(
    XnuThreadId thread, ThreadRecord& record) {
    if (record.info.failsafe) return;
    record.failsafe_saved_base_priority = record.info.base_priority;
    record.failsafe_saved_timeshare = record.info.timeshare;
    record.failsafe_saved_realtime = record.info.realtime;
    if (record.info.realtime) {
        record.info.realtime = false;
        record.info.base_priority = xnu792::scheduler::minimum_priority;
    }
    record.info.timeshare = true;
    record.info.failsafe = true;
    record.info.failsafe_release_tick = saturating_add(
        scheduler_tick_, xnu792::scheduler::failsafe_release_scheduler_ticks);
    if (record.info.state != XnuThreadState::Waiting &&
        !record.failsafe_saved_timeshare) {
        ++active_timeshare_count_;
    }
    index_failsafe(thread, record);
    recompute_priority(thread, record);
}

void XnuScheduler::release_failsafe(
    XnuThreadId thread, ThreadRecord& record) {
    if (!record.info.failsafe) return;
    unindex_failsafe(thread, record);
    if (record.info.state != XnuThreadState::Waiting &&
        record.info.timeshare != record.failsafe_saved_timeshare) {
        if (record.failsafe_saved_timeshare) {
            ++active_timeshare_count_;
        } else {
            --active_timeshare_count_;
        }
    }
    record.info.base_priority = record.failsafe_saved_base_priority;
    record.info.timeshare = record.failsafe_saved_timeshare;
    record.info.realtime = record.failsafe_saved_realtime;
    record.info.failsafe = false;
    record.info.failsafe_release_tick = 0;
    record.info.computation_metered = 0;
    record.info.remaining_quantum = quantum_for(record);
    recompute_priority(thread, record);
}

std::uint32_t XnuScheduler::timeshare_quanta() const {
    const auto processor_count = processor_run_queues_.size();
    auto run_queue_count = processor_set_run_queue_.count;
    if (run_queue_count >= processor_count) return 1;
    if (run_queue_count <= 1) {
        return static_cast<std::uint32_t>(processor_count);
    }
    return static_cast<std::uint32_t>(
        (processor_count + run_queue_count / 2U) / run_queue_count);
}

}  // namespace ilemu
