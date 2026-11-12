// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// Deliver the process workqueue kqueue through libpthread's kevent upcall.
// XNU reference: bsd/kern/kern_event.c and libpthread kern/kern_support.c.

#include "kernel/kernel.hpp"
#include "kernel/darwin_abi.hpp"
#include "pthread_priority.hpp"
#include "../workqueue_qos_probe.hpp"
#include "kernel/kevent_wire.hpp"
#include "device_state/pthread_contract.hpp"
#include <algorithm>
#include <set>
#include <limits>

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

CompatibilityKernel::WorkqueueKeventQos
CompatibilityKernel::workqueue_kevent_qos(
    const KeventRegistration& registration, MachMessageQos message_qos) const
{
    const auto& contract = shared_state_->pthread_contract;
    auto bucket = darwin::kqueue::workqueue_qos_bucket(registration.qos);
    if (contract.priority_encoding == PthreadPriorityEncoding::QueueIndex)
        return { bucket, std::nullopt };
    // Retain the existing unspecified fallback and explicit manager contract.
    if (bucket == 0U || bucket == darwin::kqueue::event_manager_qos_flag)
        return { bucket, std::nullopt };
    if (registration.filter != darwin::kqueue::filter_mach_port ||
        (registration.filter_flags & darwin::mach_message::option_receive) ==
            0U)
        return { bucket, std::nullopt };
    bucket = std::max(
        bucket, PthreadPriority::qos_bucket(contract, message_qos.requested));
    const auto override_priority =
        message_qos.override_tier == XnuQosTier::Unspecified
            ? std::nullopt
            : std::optional {
                  XnuThreadQosPolicy { message_qos.override_tier }.priority()
              };
    return { bucket, override_priority };
}

bool CompatibilityKernel::boost_workqueue_qos(
    std::uint32_t processor, std::optional<std::int32_t> priority)
{
    auto resources = pthread_runtime_.resource_qos_overrides(processor);
    const auto previous = resources.priority();
    resources.boost_workqueue(priority);
    if (previous != resources.priority()) {
        const auto effective =
            std::max(pthread_runtime_.qos_override(processor).priority(),
                resources.priority());
        if (!thread_qos_override_handler_ ||
            !thread_qos_override_handler_(processor, effective))
            return false;
    }
    pthread_runtime_.set_resource_qos_overrides(
        processor, std::move(resources));
    return true;
}

bool CompatibilityKernel::workqueue_kevent_bucket_active(
    std::uint32_t bucket) const
{
    const auto assigned = workqueue_kevent_processors_.find(bucket);
    if (assigned == workqueue_kevent_processors_.end())
        return false;
    if (bucket == darwin::kqueue::event_manager_qos_flag)
        return true;
    // A blocked worker releases its concurrency slot until it becomes runnable.
    return std::ranges::any_of(assigned->second, [&](std::uint32_t processor) {
        const auto state =
            thread_scheduling_state_query_
                ? thread_scheduling_state_query_(process_.pid, processor)
                : std::optional<XnuThreadState> { };
        return !state || *state != XnuThreadState::Waiting;
    });
}

std::optional<std::uint64_t>
CompatibilityKernel::workqueue_kevent_deadline() const
{
    if (workqueue_timer_snapshot_ &&
        workqueue_timer_snapshot_->generation == local_io_event_generation_)
        return workqueue_timer_snapshot_->deadline;
    std::optional<std::uint64_t> deadline;
    const auto queue = kqueues_.find(darwin::kqueue::workqueue_descriptor);
    if (pthread_runtime_.workqueue_open() && !workqueue_kevent_queued_ &&
        queue != kqueues_.end()) {
        for (const auto& event : queue->second) {
            if (!event.enabled || event.deferred_delete || !event.timer ||
                workqueue_kevent_bucket_active(
                    workqueue_kevent_qos(event, { }).bucket))
                continue;
            const auto next = event.timer->deadline();
            if (next && (!deadline || *next < *deadline))
                deadline = next;
        }
    }
    workqueue_timer_snapshot_ =
        TimerDeadlineSnapshot { local_io_event_generation_, deadline };
    return deadline;
}

bool CompatibilityKernel::workqueue_kevent_poll_required(
    std::span<const KeventRegistration> registrations,
    std::uint64_t io_generation, bool io_changed) const
{
    return !std::all_of(registrations.begin(), registrations.end(),
        [this, io_generation, io_changed](const auto& registration) {
            if (!registration.enabled && !registration.workqueue_servicer)
                return true;
            if (registration.deferred_delete)
                return false;
            if (registration.filter == darwin::kqueue::filter_mach_port)
                return mach_kevent_observation_current(registration);
            // Match delivery's readiness and bucket eligibility before
            // asking the workqueue to rescan unrelated I/O.
            if (registration.inactive_source_io_generation == io_generation)
                return true;
            if (const auto source = kevent_source_state(registration)) {
                if (!source->ready &&
                    registration.filter != darwin::kqueue::filter_timer)
                    registration.inactive_source_io_generation = io_generation;
                return !source->ready ||
                    workqueue_kevent_bucket_active(
                        workqueue_kevent_qos(registration, { }).bucket);
            }
            if (io_changed)
                return false;
            if (registration.filter == darwin::kqueue::filter_read ||
                registration.filter == darwin::kqueue::filter_write) {
                const auto fd = static_cast<std::uint32_t>(registration.ident);
                // Composite descriptors can expose nested Mach/timer
                // sources; polling devices also advance outside I/O notes.
                return !kqueues_.contains(fd) &&
                       !duplicated_descriptors_.contains(fd) &&
                       !descriptor_requires_host_poll(fd);
            }
            return true;
        });
}

void CompatibilityKernel::note_workqueue_kevent_transition()
{
    workqueue_kevent_redrive_ = true;
    workqueue_timer_snapshot_.reset();
    note_timer_deadline_transition();
}

bool CompatibilityKernel::request_workqueue_kevents(Cpu* requesting_cpu)
{
    workqueue_kevent_redrive_ = false;
    HostSocketRequests requests;
    std::set<std::uint32_t> visited;
    collect_host_socket_requests(darwin::kqueue::workqueue_descriptor,
        SocketReadable, requests, visited);
    update_host_socket_watches(std::numeric_limits<std::size_t>::max(), requests);
    if (!pthread_runtime_.workqueue_open() ||
        !kqueues_.contains(darwin::kqueue::workqueue_descriptor)) {
        return true;
    }
    const auto& registrations =
        kqueues_.at(darwin::kqueue::workqueue_descriptor);
    std::set<std::uint32_t> observed_buckets;
    std::map<std::uint32_t, std::optional<std::int32_t>> overrides;
    WorkqueueQosProbe qos_probe;
    for (const auto& registration : registrations) {
        MachMessageQos message_qos;
        bool mach_ready = false;
        const auto registered_bucket =
            darwin::kqueue::workqueue_qos_bucket(registration.qos);
        if (shared_state_->pthread_contract.priority_encoding !=
                PthreadPriorityEncoding::QueueIndex &&
            registered_bucket != 0U &&
            registered_bucket != darwin::kqueue::event_manager_qos_flag &&
            (registration.enabled || registration.workqueue_servicer) &&
            !registration.deferred_delete &&
            registration.filter == darwin::kqueue::filter_mach_port &&
            (registration.filter_flags &
                darwin::mach_message::option_receive) != 0U) {
            mach_ready =
                ready_mach_kevent_name(registration, &message_qos).has_value();
        }
        const auto qos = workqueue_kevent_qos(
            registration, mach_ready ? message_qos : MachMessageQos { });
        if (registration.workqueue_servicer &&
            !boost_workqueue_qos(
                *registration.workqueue_servicer, qos.override_priority))
            return false;
        if (registration.enabled && mach_ready)
            overrides[qos.bucket] =
                std::max(overrides[qos.bucket], qos.override_priority);
        const auto bucket = qos.bucket;
        if (!observed_buckets.insert(bucket).second)
            continue;
        if (workqueue_kevent_bucket_active(bucket))
            continue;
        qos_probe.add_bucket(bucket);
    }
    // The bucket's current servicer also protects ready knotes it has not
    // taken yet. An override need not belong to the knote being processed.
    for (const auto& [bucket, priority] : overrides) {
        if (const auto assigned = workqueue_kevent_processors_.find(bucket);
            assigned != workqueue_kevent_processors_.end()) {
            for (const auto processor : assigned->second)
                if (!boost_workqueue_qos(processor, priority))
                    return false;
        }
    }
    if (workqueue_kevent_queued_)
        return true;
    if (qos_probe.empty())
        return true;
    const auto ready = collect_ready_kevents(0U,
        darwin::kqueue::workqueue_descriptor, 0U, 0U,
        darwin::kqueue::WireFormat::QualityOfService, false, &qos_probe);
    if (!ready)
        return false;
    if (*ready == 0U)
        return true;

    const auto qos_bucket = *qos_probe.selected_bucket();
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
        .kevent_qos_override = overrides[qos_bucket],
    };
    if (!pthread_runtime_.enqueue_workitem(request))
        return false;
    workqueue_kevent_queued_ = true;
    note_workqueue_kevent_transition();
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
