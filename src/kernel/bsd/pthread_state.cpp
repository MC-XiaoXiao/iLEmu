// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// Maintain guest pthread registration, workqueue threads and pending
// work items.
//
// Apple public ABI/behavior references (guest profiles may differ):
// https://github.com/apple-oss-distributions/xnu/blob/xnu-1699.22.73/bsd/kern/pthread_support.c

#include "kernel/darwin_pthread_runtime.hpp"
#include <algorithm>

namespace ilemu {

bool DarwinPthreadRuntime::register_process(
    DarwinPthreadRegistration registration)
{
    if (registration_ || registration.pthread_size > maximum_pthread_size)
        return false;
    registration_ = registration;
    return true;
}

bool DarwinPthreadRuntime::enqueue_workitem(
    DarwinWorkqueueItem item, bool front)
{
    if (!workqueue_open_ || item.priority >= workqueue_priority_count_)
        return false;
    auto& queue = workitems_[item.priority];
    if (queue.size() >= maximum_workqueue_items_per_priority)
        return false;
    if (front)
        queue.push_front(item);
    else
        queue.push_back(item);
    return true;
}

bool DarwinPthreadRuntime::request_dispatch_threads(
    std::uint32_t count, std::uint32_t priority, bool overcommit,
    std::uint32_t thread_class)
{
    if (!workqueue_open_ || priority >= workqueue_priority_count_ || count == 0U)
        return false;
    auto& queue = workitems_[priority];
    if (count > maximum_workqueue_items_per_priority - queue.size())
        return false;
    for (std::uint32_t index = 0; index < count; ++index)
        queue.push_back(DarwinWorkqueueItem { 0U, priority, 0U, overcommit,
            DarwinWorkqueueDelivery::DispatchThread, thread_class });
    return true;
}

bool DarwinPthreadRuntime::has_pending_workitems() const noexcept
{
    return std::ranges::any_of(workitems_,
        [](const auto& queue) { return !queue.empty(); });
}

std::optional<DarwinWorkqueueItem> DarwinPthreadRuntime::take_workitem(
    const ActiveWorkers& active_workers,
    std::optional<std::uint32_t> returning_processor, bool requires_new_worker)
{
    if (!workqueue_open_)
        return std::nullopt;
    const auto constrained_allowed =
        can_schedule_workitem(false, returning_processor);
    // libpthread schedules overcommit requests independently of constrained
    // capacity. Leave blocked requests queued while selecting the first
    // eligible request in priority/FIFO order, including within one bucket.
    for (auto& queue : workitems_) {
        auto ready = queue.end();
        for (auto candidate = queue.begin(); candidate != queue.end(); ++candidate) {
            const auto& item = *candidate;
            std::size_t active_count = 0;
            if (item.delivery == DarwinWorkqueueDelivery::WorkItem) {
                for (std::size_t priority = 0; priority <= item.priority; ++priority)
                    active_count += active_workers.priorities[priority];
            } else {
                active_count = active_workers.classes[item.thread_class];
            }
            const auto unconstrained = item.overcommit || item.event_manager;
            if ((!unconstrained && (!constrained_allowed ||
                    active_count >= target_concurrency_[item.priority])) ||
                (requires_new_worker && !should_create_worker(
                    item.priority, unconstrained, active_count)))
                continue;
            if (ready == queue.end() || item.thread_class > ready->thread_class)
                ready = candidate;
        }
        if (ready == queue.end())
            continue;
        const auto item = *ready;
        queue.erase(ready);
        return item;
    }
    return std::nullopt;
}

bool DarwinPthreadRuntime::remove_workitem(
    std::uint32_t address, std::uint32_t priority)
{
    if (!workqueue_open_ || priority >= workqueue_priority_count_)
        return false;
    auto& queue = workitems_[priority];
    const auto item = std::find_if(
        queue.begin(), queue.end(), [address](const auto& candidate) {
            return candidate.address == address;
        });
    if (item == queue.end())
        return false;
    queue.erase(item);
    return true;
}

bool DarwinPthreadRuntime::should_create_worker(
    std::uint32_t priority, bool overcommit,
    std::size_t active_worker_count) const noexcept
{
    if (!workqueue_open_ || priority >= workqueue_priority_count_ ||
        workers_.size() >= maximum_workqueue_workers ||
        !can_schedule_workitem(overcommit))
        return false;
    return overcommit || active_worker_count < target_concurrency_[priority];
}

bool DarwinPthreadRuntime::can_schedule_workitem(bool overcommit,
    std::optional<std::uint32_t> returning_processor) const noexcept
{
    if (!workqueue_open_)
        return false;
    if (overcommit)
        return true;
    const auto constrained = std::count_if(workers_.begin(), workers_.end(),
        [returning_processor](const auto& entry) {
            return !entry.second.idle && !entry.second.overcommit &&
                   (!returning_processor || entry.first != *returning_processor);
        });
    return static_cast<std::size_t>(constrained) <
           maximum_constrained_workqueue_workers;
}

bool DarwinPthreadRuntime::add_worker(DarwinWorkqueueWorker worker)
{
    if (!workqueue_open_ || workers_.size() >= maximum_workqueue_workers)
        return false;
    return workers_.emplace(worker.processor, worker).second;
}

std::optional<DarwinWorkqueueWorker> DarwinPthreadRuntime::worker(
    std::uint32_t processor) const
{
    const auto found = workers_.find(processor);
    return found == workers_.end() ? std::nullopt
                                   : std::optional { found->second };
}

std::optional<DarwinWorkqueueWorker> DarwinPthreadRuntime::idle_worker() const
{
    const auto found = std::find_if(workers_.begin(), workers_.end(),
        [](const auto& entry) { return entry.second.idle; });
    return found == workers_.end() ? std::nullopt
                                   : std::optional { found->second };
}

std::vector<std::uint32_t>
DarwinPthreadRuntime::active_worker_processors() const
{
    std::vector<std::uint32_t> processors;
    processors.reserve(workers_.size());
    for (const auto& [processor, worker] : workers_) {
        if (!worker.idle)
            processors.push_back(processor);
    }
    return processors;
}

void DarwinPthreadRuntime::mark_worker_running(
    std::uint32_t processor, std::uint32_t priority, bool overcommit,
    std::uint32_t thread_class)
{
    if (const auto found = workers_.find(processor); found != workers_.end()) {
        found->second.idle = false;
        found->second.priority = priority;
        found->second.overcommit = overcommit;
        found->second.thread_class = thread_class;
    }
}

void DarwinPthreadRuntime::park_worker(std::uint32_t processor)
{
    if (const auto found = workers_.find(processor); found != workers_.end())
        found->second.idle = true;
}

void DarwinPthreadRuntime::remove_worker(std::uint32_t processor)
{
    workers_.erase(processor);
    reset_qos_overrides(processor);
}

DarwinPthreadQosOverride DarwinPthreadRuntime::qos_override(
    std::uint32_t processor) const noexcept
{
    const auto found = qos_overrides_.find(processor);
    return found == qos_overrides_.end() ? DarwinPthreadQosOverride { }
                                        : found->second;
}

void DarwinPthreadRuntime::set_qos_override(
    std::uint32_t processor, DarwinPthreadQosOverride state)
{
    if (state.empty())
        qos_overrides_.erase(processor);
    else
        qos_overrides_.insert_or_assign(processor, state);
}

DarwinPthreadResourceQosOverrides DarwinPthreadRuntime::resource_qos_overrides(
    std::uint32_t processor) const
{
    const auto found = resource_qos_overrides_.find(processor);
    return found == resource_qos_overrides_.end() ? DarwinPthreadResourceQosOverrides { }
                                                : found->second;
}

void DarwinPthreadRuntime::set_resource_qos_overrides(
    std::uint32_t processor, DarwinPthreadResourceQosOverrides state)
{
    if (state.empty())
        resource_qos_overrides_.erase(processor);
    else
        resource_qos_overrides_.insert_or_assign(processor, std::move(state));
}

std::optional<std::int32_t> DarwinPthreadRuntime::effective_qos_override_priority(
    std::uint32_t processor) const noexcept
{
    auto result = qos_override(processor).priority();
    if (const auto resource = resource_qos_overrides_.find(processor);
        resource != resource_qos_overrides_.end())
        result = std::max(result, resource->second.priority());
    return result;
}

void DarwinPthreadRuntime::reset_qos_overrides(std::uint32_t processor)
{
    qos_overrides_.erase(processor);
    resource_qos_overrides_.erase(processor);
}

bool DarwinPthreadRuntime::set_target_concurrency(
    std::uint32_t priority, std::uint32_t concurrency)
{
    if (!workqueue_open_ || priority > workqueue_priority_count_)
        return false;
    if (priority == workqueue_priority_count_) {
        for (std::uint32_t index = 0; index < workqueue_priority_count_;
             ++index) {
            target_concurrency_[index] = concurrency;
        }
    } else {
        target_concurrency_[priority] = concurrency;
    }
    return true;
}

void DarwinPthreadRuntime::reset_workqueue() noexcept
{
    workqueue_open_ = false;
    workqueue_priority_count_ = 0U;
    for (auto& queue : workitems_)
        queue.clear();
    workers_.clear();
    target_concurrency_.fill(0);
}

} // namespace ilemu
