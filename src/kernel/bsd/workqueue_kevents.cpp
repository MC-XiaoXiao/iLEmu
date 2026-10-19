// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// Deliver the process workqueue kqueue through libpthread's kevent upcall.
// XNU reference: bsd/kern/kern_event.c and libpthread kern/kern_support.c.

#include "kernel/kernel.hpp"
#include "kernel/kevent_wire.hpp"
#include "device_state/pthread_contract.hpp"
#include <algorithm>
#include <set>

namespace ilemu {
namespace {
    constexpr std::uint32_t upcall_event_capacity = 16U;
    constexpr std::uint32_t upcall_data_capacity = 32U * 1024U;
    constexpr std::uint32_t upcall_stack_gap = 16U;
    constexpr std::uint32_t default_thread_class = 0x15U;
    constexpr std::uint32_t default_workqueue_priority = 1U;

    struct WorkqueueClass {
        std::uint32_t thread_class;
        std::uint32_t priority;
    };

    [[nodiscard]] WorkqueueClass class_for_kevent_qos(
        const PthreadContract& contract, std::uint32_t qos_bucket)
    {
        if (contract.priority_encoding == PthreadPriorityEncoding::QueueIndex) {
            return { default_thread_class, default_workqueue_priority };
        }
        // The workqueue upcall's low 16 flag bits contain a public QoS
        // class, while the kernel scheduler uses a compact queue index.
        // Both must describe the QoS bucket selected for this kevent.
        auto class_bucket = qos_bucket;
        if (contract.priority_encoding ==
            PthreadPriorityEncoding::FineClassBitsWithMaintenance) {
            if (class_bucket == 0x0100U)
                return { 0x05U, 3U }; // maintenance
            class_bucket >>= 1U;
        }
        switch (class_bucket) {
        case 0x1000U: return { 0x21U, 0U }; // user interactive
        case 0x0800U: return { 0x19U, 0U }; // user initiated
        case 0x0400U: return { 0x15U, 1U }; // default
        case 0x0200U: return { 0x11U, 2U }; // utility
        case 0x0100U: return { 0x09U, 3U }; // background
        default: return { default_thread_class, default_workqueue_priority };
        }
    }
}

bool CompatibilityKernel::request_workqueue_kevents(Cpu* requesting_cpu)
{
    if (!pthread_runtime_.workqueue_open() || workqueue_kevent_queued_ ||
        !kqueues_.contains(darwin::kqueue::workqueue_descriptor)) {
        return true;
    }
    const auto& registrations =
        kqueues_.at(darwin::kqueue::workqueue_descriptor);
    std::set<std::uint32_t> observed_buckets;
    std::optional<std::uint32_t> selected_bucket;
    const auto processor_is_active = [&](std::uint32_t processor) {
        // A worker blocked in a guest kernel wait no longer occupies its
        // workqueue concurrency slot. Keep delivering ready kevents through
        // another worker while that handler waits for a synchronous reply.
        const auto state = thread_scheduling_state_query_
            ? thread_scheduling_state_query_(process_.pid, processor)
            : std::optional<XnuThreadState> { };
        return !state || *state != XnuThreadState::Waiting;
    };
    for (const auto& registration : registrations) {
        const auto bucket =
            darwin::kqueue::workqueue_qos_bucket(registration.qos);
        const auto assigned = workqueue_kevent_processors_.find(bucket);
        const auto manager_bucket =
            bucket == darwin::kqueue::event_manager_qos_flag;
        if (!observed_buckets.insert(bucket).second ||
            (assigned != workqueue_kevent_processors_.end() &&
                (manager_bucket ||
                    std::ranges::any_of(assigned->second,
                        processor_is_active))))
            continue;
        const auto ready = collect_ready_kevents(0U,
            darwin::kqueue::workqueue_descriptor, 0U, 0U,
            darwin::kqueue::WireFormat::QualityOfService, false, nullptr,
            nullptr, bucket);
        if (!ready)
            return false;
        if (*ready != 0U) {
            selected_bucket = bucket;
            break;
        }
    }
    if (!selected_bucket)
        return true;

    const auto qos_bucket = *selected_bucket;
    const bool event_manager =
        qos_bucket == darwin::kqueue::event_manager_qos_flag;
    const auto workqueue_class = class_for_kevent_qos(
        shared_state_->pthread_contract, qos_bucket);

    const DarwinWorkqueueItem request {
        .address = 0U,
        .priority = workqueue_class.priority,
        .affinity = 0U,
        .overcommit = false,
        .delivery = DarwinWorkqueueDelivery::Kevent,
        .thread_class = workqueue_class.thread_class,
        .event_manager = event_manager,
        .kevent_qos_bucket = qos_bucket,
    };
    if (!pthread_runtime_.enqueue_workitem(request))
        return false;
    workqueue_kevent_queued_ = true;
    return service_bsd_workqueue(requesting_cpu);
}

bool CompatibilityKernel::prepare_workqueue_kevent_upcall(
    std::array<std::uint32_t, 16>& state,
    const DarwinWorkqueueWorker& worker,
    const DarwinWorkqueueItem& item)
{
    const KeventWireFormat wire {
        darwin::kqueue::WireFormat::QualityOfService };
    const auto list_size = upcall_event_capacity * wire.size();
    if (worker.pthread_address < worker.stack_bottom + list_size +
            upcall_data_capacity +
            upcall_stack_gap) {
        return false;
    }
    const auto list_address = worker.pthread_address - list_size;
    const auto data_address = list_address - upcall_data_capacity;
    KeventDataBuffer data_buffer { data_address, upcall_data_capacity };
    const auto events = collect_ready_kevents(worker.processor,
        darwin::kqueue::workqueue_descriptor, list_address,
        upcall_event_capacity, darwin::kqueue::WireFormat::QualityOfService,
        false, nullptr, &data_buffer, item.kevent_qos_bucket);
    if (!events)
        return false;
    state[3] = *events == 0U ? 0U : list_address;
    state[5] = *events;
    const auto stack_top = data_buffer.remaining == upcall_data_capacity
        ? list_address : data_address;
    state[13] = (stack_top - upcall_stack_gap) & ~std::uint32_t { 0x0fU };
    output_.write("[pthread] workqueue kevents pid=" +
        std::to_string(process_.pid) + " count=" +
        std::to_string(*events) + "\n");
    return true;
}

} // namespace ilemu
