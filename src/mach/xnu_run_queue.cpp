// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// Priority bitmap/FIFO and deadline run queues. As in XNU run_queue_enqueue
// and run_queue_dequeue, a registered thread owns its queue storage; dispatch
// uses Dynarmic's existing mcl intrusive list without allocating or parking
// nodes in an intermediate list. Policy and CPU accounting live
// in xnu_scheduler.cpp.
// https://github.com/apple-oss-distributions/xnu/blob/xnu-12377.121.6/osfmk/kern/sched_prim.c

#include "mach/xnu_scheduler.hpp"

#include <algorithm>
#include <bit>
#include <stdexcept>
#include <utility>

namespace ilemu {

void XnuScheduler::enqueue(ThreadRecord& record, QueuePosition position)
{
    const auto thread = record.id;
    if (record.queued)
        return;
    const auto priority = record.info.scheduled_priority;
    auto& run_queue =
        record.info.bound_processor
            ? processor_run_queues_.at(*record.info.bound_processor)
            : processor_set_run_queue_;
    auto& queue = run_queue.queues[static_cast<std::size_t>(priority)];
    record.enqueue_sequence = next_enqueue_sequence_++;
    if (record.info.realtime &&
        priority >= xnu::scheduler::realtime_queue_priority) {
        const auto realtime_key =
            RealtimeQueueKey { record.info.realtime_deadline,
                record.enqueue_sequence, thread };
        if (record.realtime_node.empty()) {
            run_queue.realtime_order.insert(realtime_key);
        } else {
            record.realtime_node.value() = realtime_key;
            run_queue.realtime_order.insert(std::move(record.realtime_node));
        }
        record.realtime_queue_key = realtime_key;
        queue.push_back(&record);
    } else {
        queue.insert(position == QueuePosition::Front ? queue.begin() : queue.end(),
            &record);
    }
    record.queued = true;
    record.queued_priority = priority;
    record.front_continuation = position == QueuePosition::Front;
    record.queued_processor = record.info.bound_processor;
    if (dispatch_diagnostics_enabled_ &&
        record.enqueued_at == std::chrono::steady_clock::time_point { }) {
        record.enqueued_at = std::chrono::steady_clock::now();
    }
    ++runnable_count_;
    ++run_queue.count;
    ++run_queue.counts[static_cast<std::size_t>(priority)];
    run_queue.bitmap[static_cast<std::size_t>(priority) / 32U] |=
        std::uint32_t { 1 } << (static_cast<std::uint32_t>(priority) % 32U);
    run_queue.high_queue = std::max(run_queue.high_queue, priority);
}

std::optional<XnuScheduler::QueueCandidate> XnuScheduler::candidate_for_queue(
    const RunQueue& run_queue, bool local) const
{
    if (run_queue.count == 0)
        return std::nullopt;
    const auto& record = peek_highest(run_queue);
    const auto thread = record.id;
    if (!record.queued || record.queued_priority != run_queue.high_queue ||
        record.queued_processor.has_value() != local) {
        throw std::logic_error { "XNU run queue candidate is inconsistent" };
    }
    return QueueCandidate { thread, record.info.scheduled_priority,
        record.info.realtime_deadline, record.enqueue_sequence,
        record.info.realtime, record.front_continuation, local };
}

bool XnuScheduler::candidate_is_better(
    const QueueCandidate& left, const QueueCandidate& right)
{
    if (left.priority != right.priority)
        return left.priority > right.priority;
    if (left.realtime != right.realtime)
        return left.realtime;
    if (left.realtime && left.realtime_deadline != right.realtime_deadline) {
        return left.realtime_deadline < right.realtime_deadline;
    }
    // QueuePosition::Front is the scheduler's continuation contract.  The
    // local and processor-set queues are selected as one ordered set, so a
    // plain enqueue age comparison would let an older global thread displace
    // a thread that still owns the remainder of its current quantum.
    if (left.front_continuation != right.front_continuation)
        return left.front_continuation;
    if (left.enqueue_sequence != right.enqueue_sequence)
        return left.enqueue_sequence < right.enqueue_sequence;
    if (left.local != right.local)
        return left.local;
    return left.thread < right.thread;
}

XnuScheduler::RunQueue* XnuScheduler::selected_run_queue(std::size_t processor)
{
    if (processor >= processor_run_queues_.size())
        return nullptr;
    auto& local_run_queue = processor_run_queues_[processor];
    const auto local = candidate_for_queue(local_run_queue, true);
    const auto global = candidate_for_queue(processor_set_run_queue_, false);
    if (!local)
        return global ? &processor_set_run_queue_ : nullptr;
    if (!global || candidate_is_better(*local, *global))
        return &local_run_queue;
    return &processor_set_run_queue_;
}

const XnuScheduler::RunQueue* XnuScheduler::selected_run_queue(
    std::size_t processor) const
{
    if (processor >= processor_run_queues_.size())
        return nullptr;
    const auto& local_run_queue = processor_run_queues_[processor];
    const auto local = candidate_for_queue(local_run_queue, true);
    const auto global = candidate_for_queue(processor_set_run_queue_, false);
    if (!local)
        return global ? &processor_set_run_queue_ : nullptr;
    if (!global || candidate_is_better(*local, *global))
        return &local_run_queue;
    return &processor_set_run_queue_;
}

XnuScheduler::ThreadRecord& XnuScheduler::peek_highest(RunQueue& run_queue)
{
    // The shared lookup only observes state; this overload is used by dispatch
    // on the owning mutable scheduler to avoid looking up the selected ID again.
    return const_cast<ThreadRecord&>(std::as_const(*this).peek_highest(run_queue));
}

const XnuScheduler::ThreadRecord& XnuScheduler::peek_highest(
    const RunQueue& run_queue) const
{
    if (run_queue.count == 0 ||
        run_queue.high_queue < xnu::scheduler::minimum_priority) {
        throw std::logic_error { "cannot peek an empty XNU run queue" };
    }
    const auto priority = run_queue.high_queue;
    if (priority >= xnu::scheduler::realtime_queue_priority) {
        if (run_queue.realtime_order.empty()) {
            throw std::logic_error {
                "XNU realtime queue index is inconsistent"
            };
        }
        const auto thread = run_queue.realtime_order.begin()->thread;
        const auto iterator = threads_.find(thread);
        if (iterator == threads_.end() || !iterator->second.queued ||
            iterator->second.queued_priority != priority) {
            throw std::logic_error {
                "XNU realtime queue index is inconsistent"
            };
        }
        return iterator->second;
    }
    const auto& queue = run_queue.queues[static_cast<std::size_t>(priority)];
    if (queue.empty()) {
        throw std::logic_error { "XNU run queue bitmap is inconsistent" };
    }
    return queue.front();
}

std::optional<XnuThreadId> XnuScheduler::peek_next_for_processor(
    std::size_t processor) const
{
    const auto* selected_queue = selected_run_queue(processor);
    if (selected_queue == nullptr)
        return std::nullopt;
    return peek_highest(*selected_queue).id;
}

void XnuScheduler::remove_from_queue(XnuThreadId, ThreadRecord& record)
{
    if (!record.queued)
        return;
    const auto priority = record.queued_priority;
    auto& run_queue = record.queued_processor
                          ? processor_run_queues_.at(*record.queued_processor)
                          : processor_set_run_queue_;
    auto& queue = run_queue.queues[static_cast<std::size_t>(priority)];
    queue.erase(record);
    if (record.realtime_queue_key) {
        record.realtime_node =
            run_queue.realtime_order.extract(*record.realtime_queue_key);
        record.realtime_queue_key.reset();
    }
    record.queued = false;
    record.queued_processor.reset();
    --runnable_count_;
    --run_queue.count;
    --run_queue.counts[static_cast<std::size_t>(priority)];
    if (queue.empty()) {
        run_queue.bitmap[static_cast<std::size_t>(priority) / 32U] &=
            ~(std::uint32_t { 1 }
                << (static_cast<std::uint32_t>(priority) % 32U));
        if (priority == run_queue.high_queue)
            refresh_high_queue(run_queue);
    }
}

void XnuScheduler::refresh_high_queue(RunQueue& run_queue)
{
    run_queue.high_queue = -1;
    for (std::size_t word_index = run_queue.bitmap.size(); word_index-- > 0;) {
        const auto word = run_queue.bitmap[word_index];
        if (word != 0) {
            const auto highest_bit = static_cast<std::size_t>(
                31U - static_cast<unsigned>(std::countl_zero(word)));
            run_queue.high_queue =
                static_cast<std::int32_t>(word_index * 32U + highest_bit);
            return;
        }
    }
}

} // namespace ilemu
