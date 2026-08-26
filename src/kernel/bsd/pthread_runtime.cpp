#include "ilemu/darwin_pthread_runtime.hpp"

#include "ilemu/darwin_abi.hpp"
#include "ilemu/darwin_kernel_profile.hpp"
#include "ilemu/kernel.hpp"

#include "../mach/support.hpp"
#include "support.hpp"

#include <algorithm>
#include <array>
#include <limits>

namespace ilemu {
namespace {

    constexpr std::uint32_t workqueue_stack_size = 512U * 1024U;
    constexpr std::uint32_t pthread_dynamic_base = 0x1000'0000U;
    // Darwin 10.4 ARM32 passes the embedded TSD base at this pthread_t
    // member to thread_set_tsd_base. Darwin 10.3 keeps TPIDRURO equal to
    // pthread_t itself, so select this offset through the ABI Profile.
    constexpr std::uint32_t pthread_tsd_base_offset = 0x48U;

    [[nodiscard]] bool supports_bsdthread_register_v1(
        DarwinPthreadAbiProfile profile) noexcept
    {
        return profile == DarwinPthreadAbiProfile::BsdThreadRegisterV1 ||
               profile == DarwinPthreadAbiProfile::BsdThreadRegisterV1TsdBase;
    }

    [[nodiscard]] std::uint32_t thread_pointer_for_pthread(
        DarwinPthreadAbiProfile profile, std::uint32_t pthread_address) noexcept
    {
        if (profile == DarwinPthreadAbiProfile::BsdThreadRegisterV1TsdBase)
            return pthread_address + pthread_tsd_base_offset;
        return pthread_address;
    }

    std::uint32_t pthread_start_cpsr(std::uint32_t entry)
    {
        return (entry & 1U) != 0U ? std::uint32_t { 0x30U }
                                  : std::uint32_t { 0x10U };
    }

    std::array<std::uint32_t, 16> workqueue_thread_state(
        const DarwinPthreadRegistration& registration,
        const DarwinWorkqueueWorker& worker, const DarwinWorkqueueItem& item,
        bool reuse)
    {
        std::array<std::uint32_t, 16> state { };
        state[0] = worker.pthread_address;
        state[1] = worker.port_name;
        state[2] = worker.stack_bottom;
        state[3] = item.address;
        state[4] = reuse ? 1U : 0U;
        state[5] = 0;
        state[13] = worker.pthread_address & ~std::uint32_t { 0x0fU };
        state[15] = registration.workqueue_thread_start & ~std::uint32_t { 1U };
        return state;
    }

} // namespace

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
    if (!workqueue_open_ || item.priority >= workqueue_priority_count)
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

std::optional<DarwinWorkqueueItem> DarwinPthreadRuntime::take_workitem()
{
    for (auto& queue : workitems_) {
        if (queue.empty())
            continue;
        auto item = queue.front();
        queue.pop_front();
        return item;
    }
    return std::nullopt;
}

bool DarwinPthreadRuntime::remove_workitem(
    std::uint32_t address, std::uint32_t priority)
{
    if (!workqueue_open_ || priority >= workqueue_priority_count)
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
    if (!workqueue_open_ || priority >= workqueue_priority_count ||
        workers_.size() >= maximum_workqueue_workers)
        return false;
    if (overcommit)
        return true;
    return active_worker_count < target_concurrency_[priority];
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
    std::uint32_t processor, std::uint32_t priority)
{
    if (const auto found = workers_.find(processor); found != workers_.end()) {
        found->second.idle = false;
        found->second.priority = priority;
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
}

bool DarwinPthreadRuntime::set_target_concurrency(
    std::uint32_t priority, std::uint32_t concurrency)
{
    if (!workqueue_open_ || priority > workqueue_priority_count)
        return false;
    if (priority == workqueue_priority_count) {
        target_concurrency_.fill(concurrency);
    } else {
        target_concurrency_[priority] = concurrency;
    }
    return true;
}

void DarwinPthreadRuntime::reset_workqueue() noexcept
{
    workqueue_open_ = false;
    for (auto& queue : workitems_)
        queue.clear();
    workers_.clear();
    target_concurrency_.fill(0);
}

bool CompatibilityKernel::service_bsd_workqueue(Cpu& cpu)
{
    const auto& registration = pthread_runtime_.registration();
    if (!registration || !pthread_runtime_.workqueue_open())
        return false;
    const auto pthread_profile =
        shared_state_->darwin_kernel_identity.pthread_abi;

    const auto idle_worker = pthread_runtime_.idle_worker();
    const auto next_item = pthread_runtime_.take_workitem();
    if (!next_item)
        return true;
    std::size_t active_worker_count { };
    for (const auto processor :
        pthread_runtime_.active_worker_processors()) {
        const auto scheduling_state =
            thread_scheduling_state_query_
                ? thread_scheduling_state_query_(process_.pid, processor)
                : std::optional<XnuThreadState> { };
        // A worker blocked in a Mach/BSD wait is still assigned to the
        // workqueue, but XNU removes it from the active concurrency count.
        // Missing scheduler information is conservatively treated as active.
        if (!scheduling_state ||
            *scheduling_state != XnuThreadState::Waiting) {
            ++active_worker_count;
        }
    }
    if (!idle_worker && !pthread_runtime_.should_create_worker(
                            next_item->priority, next_item->overcommit,
                            active_worker_count)) {
        static_cast<void>(pthread_runtime_.enqueue_workitem(*next_item, true));
        return true;
    }

    if (idle_worker) {
        const auto state = workqueue_thread_state(
            *registration, *idle_worker, *next_item, true);
        const auto cpsr =
            pthread_start_cpsr(registration->workqueue_thread_start);
        darwin::arm_thread::GeneralState guest_state { };
        std::copy(state.begin(), state.end(), guest_state.begin());
        guest_state[darwin::arm_thread::cpsr_index] = cpsr;
        const auto updated =
            thread_state_update_handler_ && thread_wake_handler_ &&
            thread_state_update_handler_(
                process_.pid, idle_worker->processor, guest_state) &&
            (!thread_pointer_update_handler_ ||
                thread_pointer_update_handler_(process_.pid,
                    idle_worker->processor,
                    thread_pointer_for_pthread(
                        pthread_profile, idle_worker->pthread_address)));
        const auto wake_result =
            updated ? thread_wake_handler_(process_.pid, idle_worker->processor)
                    : XnuThreadWakeResult { };
        if (!updated || !wake_result.handled) {
            static_cast<void>(
                pthread_runtime_.enqueue_workitem(*next_item, true));
            pthread_runtime_.park_worker(idle_worker->processor);
            return false;
        }
        pthread_runtime_.mark_worker_running(
            idle_worker->processor, next_item->priority);
        output_.write(
            "[pthread] workqueue reuse pid=" + std::to_string(process_.pid) +
            " slot=" + std::to_string(idle_worker->processor) +
            " item=" + std::to_string(next_item->address) +
            " priority=" + std::to_string(next_item->priority) + "\n");
        if (wake_result.preemption_needed && scheduler_preemption_query_ &&
            scheduler_preemption_query_(cpu.processor_id())) {
            cpu.request_guest_preemption();
        }
        return true;
    }

    constexpr std::uint32_t guard_size = AddressSpace::page_size;
    const auto total_size64 = static_cast<std::uint64_t>(guard_size) +
                              workqueue_stack_size + registration->pthread_size;
    if (total_size64 > std::numeric_limits<std::uint32_t>::max()) {
        static_cast<void>(pthread_runtime_.enqueue_workitem(*next_item, true));
        return false;
    }
    const auto total_size = static_cast<std::uint32_t>(total_size64);
    const auto base = mach_support::find_free_guest_region(
        memory_, pthread_dynamic_base, total_size);
    if (!base || !memory_.map(*base, guard_size, MemoryPermission::None) ||
        !memory_.map(*base + guard_size, total_size - guard_size,
            MemoryPermission::Read | MemoryPermission::Write)) {
        if (base)
            static_cast<void>(memory_.unmap(*base, total_size));
        static_cast<void>(pthread_runtime_.enqueue_workitem(*next_item, true));
        return false;
    }

    DarwinWorkqueueWorker worker {
        .allocation_base = *base,
        .allocation_size = total_size,
        .pthread_address = *base + guard_size + workqueue_stack_size,
        .stack_bottom = *base + guard_size,
        .priority = next_item->priority,
    };
    auto state =
        workqueue_thread_state(*registration, worker, *next_item, false);
    const auto cpsr = pthread_start_cpsr(registration->workqueue_thread_start);
    std::uint32_t create_error { };
    const auto created = create_guest_thread(state, cpsr, false, create_error);
    if (!created) {
        static_cast<void>(memory_.unmap(*base, total_size));
        static_cast<void>(pthread_runtime_.enqueue_workitem(*next_item, true));
        return false;
    }
    worker.processor = static_cast<std::uint32_t>(created->processor);
    worker.port_name = created->port_name;
    state = workqueue_thread_state(*registration, worker, *next_item, false);
    darwin::arm_thread::GeneralState guest_state { };
    std::copy(state.begin(), state.end(), guest_state.begin());
    guest_state[darwin::arm_thread::cpsr_index] = cpsr;
    if (!thread_state_update_handler_ ||
        !thread_state_update_handler_(
            process_.pid, worker.processor, guest_state) ||
        (thread_pointer_update_handler_ &&
            !thread_pointer_update_handler_(
                process_.pid, worker.processor,
                thread_pointer_for_pthread(
                    pthread_profile, worker.pthread_address))) ||
        !pthread_runtime_.add_worker(worker)) {
        if (thread_terminate_handler_)
            static_cast<void>(
                thread_terminate_handler_(process_.pid, worker.processor));
        static_cast<void>(memory_.unmap(*base, total_size));
        static_cast<void>(pthread_runtime_.enqueue_workitem(*next_item, true));
        return false;
    }

    output_.write(
        "[pthread] workqueue create pid=" + std::to_string(process_.pid) +
        " slot=" + std::to_string(worker.processor) +
        " item=" + std::to_string(next_item->address) +
        " pthread=" + std::to_string(worker.pthread_address) +
        " priority=" + std::to_string(next_item->priority) + "\n");
    if (scheduler_preemption_query_ &&
        scheduler_preemption_query_(cpu.processor_id())) {
        cpu.request_guest_preemption();
    }
    return true;
}

bool CompatibilityKernel::dispatch_bsd_pthread(Cpu& cpu, std::uint32_t number)
{
    const auto profile = shared_state_->darwin_kernel_identity.pthread_abi;
    if (!supports_bsdthread_register_v1(profile))
        return false;

    auto& registers = cpu.registers();
    switch (number) {
    case 360: { // bsdthread_create, Darwin 10 ARM32 v1
        constexpr std::uint32_t guard_size = AddressSpace::page_size;
        constexpr std::uint32_t dynamic_base = 0x1000'0000U;
        constexpr std::uint32_t custom_stack = 0x0100'0000U;
        const auto& registration = pthread_runtime_.registration();
        if (!registration) {
            bsd_error(cpu, bsd_support::invalid_argument);
            return true;
        }

        const auto user_function = registers[0];
        const auto user_argument = registers[1];
        const auto stack_argument = registers[2];
        const auto pthread_argument = registers[3];
        const auto flags = registers[4];
        std::uint32_t pthread_address = pthread_argument;
        std::uint32_t stack_pointer = stack_argument;
        std::optional<std::pair<std::uint32_t, std::uint32_t>> allocation;

        if ((flags & custom_stack) == 0) {
            const auto total_size64 = static_cast<std::uint64_t>(guard_size) +
                                      stack_argument +
                                      registration->pthread_size;
            if (stack_argument == 0 ||
                total_size64 > std::numeric_limits<std::uint32_t>::max()) {
                bsd_error(cpu, bsd_support::invalid_argument);
                return true;
            }
            const auto total_size = static_cast<std::uint32_t>(total_size64);
            const auto base = mach_support::find_free_guest_region(
                memory_, dynamic_base, total_size);
            if (!base ||
                !memory_.map(*base, guard_size, MemoryPermission::None) ||
                !memory_.map(*base + guard_size, total_size - guard_size,
                    MemoryPermission::Read | MemoryPermission::Write)) {
                if (base)
                    static_cast<void>(memory_.unmap(*base, total_size));
                bsd_error(cpu, darwin::error::no_memory);
                return true;
            }
            allocation = std::pair { *base, total_size };
            pthread_address = *base + guard_size + stack_argument;
            stack_pointer = pthread_address;
        } else if (stack_pointer == 0 || pthread_address == 0) {
            bsd_error(cpu, bsd_support::invalid_argument);
            return true;
        }

        std::array<std::uint32_t, 16> state { };
        state[0] = pthread_address;
        state[2] = user_function;
        state[3] = user_argument;
        state[4] = stack_argument;
        state[5] = flags;
        state[13] = stack_pointer & ~std::uint32_t { 0x0fU };
        state[15] = registration->thread_start & ~std::uint32_t { 1U };
        const auto cpsr = (registration->thread_start & 1U) != 0U
                              ? std::uint32_t { 0x30U }
                              : std::uint32_t { 0x10U };
        std::uint32_t create_error { };
        const auto created =
            create_guest_thread(state, cpsr, false, create_error);
        if (!created) {
            if (allocation)
                static_cast<void>(
                    memory_.unmap(allocation->first, allocation->second));
            bsd_error(cpu, create_error == darwin::mach::resource_shortage
                               ? darwin::error::no_memory
                               : bsd_support::invalid_argument);
            return true;
        }

        state[1] = created->port_name;
        if (!thread_state_update_handler_ ||
            !thread_state_update_handler_(process_.pid,
                static_cast<std::uint32_t>(created->processor),
                darwin::arm_thread::GeneralState { state[0], state[1], state[2],
                    state[3], state[4], state[5], state[6], state[7], state[8],
                    state[9], state[10], state[11], state[12], state[13],
                    state[14], state[15], cpsr }) ||
            (thread_pointer_update_handler_ &&
                !thread_pointer_update_handler_(process_.pid,
                    static_cast<std::uint32_t>(created->processor),
                    thread_pointer_for_pthread(profile, pthread_address)))) {
            if (thread_terminate_handler_)
                static_cast<void>(thread_terminate_handler_(
                    process_.pid, created->processor));
            if (allocation)
                static_cast<void>(
                    memory_.unmap(allocation->first, allocation->second));
            bsd_error(cpu, bsd_support::invalid_argument);
            return true;
        }
        output_.write("[pthread] create pid=" + std::to_string(process_.pid) +
                      " slot=" + std::to_string(created->processor) +
                      " start=" + std::to_string(registration->thread_start) +
                      " function=" + std::to_string(user_function) +
                      " pthread=" + std::to_string(pthread_address) + "\n");
        bsd_success(cpu, pthread_address);
        if (scheduler_preemption_query_ &&
            scheduler_preemption_query_(cpu.processor_id()))
            cpu.request_guest_preemption();
        return true;
    }
    case 361: { // bsdthread_terminate, Darwin 10 ARM32 v1
        const auto stack_address = registers[0];
        const auto stack_size = registers[1];
        const auto thread_port = registers[2];
        const auto semaphore = registers[3];
        if (stack_address != 0 && stack_size != 0 &&
            !memory_.unmap(stack_address, stack_size)) {
            bsd_error(cpu, bsd_support::invalid_argument);
            return true;
        }

        const auto processor = static_cast<std::uint32_t>(cpu.processor_id());
        const auto thread_object = thread_object_for_processor(processor);
        if (thread_terminate_handler_ &&
            !thread_terminate_handler_(process_.pid, processor)) {
            bsd_error(cpu, bsd_support::invalid_argument);
            return true;
        }

        pthread_runtime_.remove_worker(processor);
        thread_ports_.erase(processor);
        pending_mach_receives_.erase(processor);
        std::optional<WokenThread> woken_thread;
        std::uint32_t semaphore_result = darwin::mach::success;
        {
            std::lock_guard mach_lock { shared_state_->mach_mutex };
            if (thread_object) {
                process_.thread_disk_io_policies.erase(*thread_object);
                if (auto task = shared_state_->task_thread_port_objects.find(
                        process_.pid);
                    task != shared_state_->task_thread_port_objects.end()) {
                    task->second.erase(processor);
                    if (task->second.empty())
                        shared_state_->task_thread_port_objects.erase(task);
                }
                mach_support::terminate_receive_object_locked(
                    *shared_state_, *thread_object);
            }
            if (semaphore != xnu792::ipc::null_name) {
                semaphore_result = signal_semaphore_locked(
                    semaphore, false, true, &woken_thread);
            }
            if (thread_port != xnu792::ipc::null_name &&
                thread_port != xnu792::ipc::dead_name) {
                const auto entry = shared_state_->mach_namespaces.lookup(
                    process_.pid, thread_port);
                if (entry) {
                    const auto has = [&](xnu792::ipc::Right right) {
                        return (entry->type & xnu792::ipc::type_mask(right)) !=
                               0;
                    };
                    const auto right = has(xnu792::ipc::Right::Send)
                                           ? xnu792::ipc::Right::Send
                                       : has(xnu792::ipc::Right::SendOnce)
                                           ? xnu792::ipc::Right::SendOnce
                                           : xnu792::ipc::Right::DeadName;
                    static_cast<void>(
                        mach_support::modify_port_references_locked(
                            *shared_state_, process_.pid, thread_port, right,
                            -1));
                }
            }
        }
        wake_thread_and_maybe_preempt(cpu, woken_thread);
        output_.write(
            "[pthread] terminate pid=" + std::to_string(process_.pid) +
            " slot=" + std::to_string(processor) +
            " stack=" + std::to_string(stack_address) +
            " size=" + std::to_string(stack_size) + "\n");
        if (semaphore_result != darwin::mach::success) {
            bsd_error(cpu, bsd_support::invalid_argument);
        } else {
            bsd_success(cpu, 0);
        }
        cpu.halt(Dynarmic::HaltReason::UserDefined1);
        return true;
    }
    case 366: { // bsdthread_register, Darwin 10 ARM32 v1
        const DarwinPthreadRegistration registration { registers[0],
            registers[1], registers[2], registers[3], registers[4],
            registers[5] };
        if (!pthread_runtime_.register_process(registration)) {
            bsd_error(cpu, bsd_support::invalid_argument);
            return true;
        }
        output_.write(
            "[pthread] register pid=" + std::to_string(process_.pid) +
            " pthread-size=" + std::to_string(registration.pthread_size) +
            " dispatch-queue-offset=" +
            std::to_string(registration.dispatch_queue_offset) + "\n");
        bsd_success(cpu, 0);
        return true;
    }
    case 367: { // workq_open, Darwin 10 ARM32 v1
        if (!pthread_runtime_.open_workqueue(virtual_processor_count_)) {
            bsd_error(cpu, bsd_support::invalid_argument);
            return true;
        }
        output_.write("[pthread] workqueue open pid=" +
                      std::to_string(process_.pid) + "\n");
        bsd_success(cpu, 0);
        return true;
    }
    case 368: { // workq_kernreturn, Darwin 10 ARM32 v1
        constexpr std::uint32_t queue_add = 1U;
        constexpr std::uint32_t queue_remove = 2U;
        constexpr std::uint32_t thread_return = 4U;
        constexpr std::uint32_t thread_set_concurrency = 8U;
        if (!pthread_runtime_.workqueue_open()) {
            bsd_error(cpu, bsd_support::invalid_argument);
            return true;
        }

        const auto operation = registers[0];
        const auto item_address = registers[1];
        // workq_kernreturn's Darwin 10 ABI is (options, item, affinity, prio).
        // Keep affinity opaque and decode the queue/overcommit bits from prio.
        const auto affinity = registers[2];
        const auto raw_priority = registers[3];
        const auto priority =
            raw_priority & ~DarwinPthreadRuntime::workqueue_overcommit;
        if (operation == queue_add) {
            const auto overcommit =
                (raw_priority & DarwinPthreadRuntime::workqueue_overcommit) !=
                0U;
            if (priority >= DarwinPthreadRuntime::workqueue_priority_count ||
                !pthread_runtime_.enqueue_workitem(DarwinWorkqueueItem {
                    item_address, priority, affinity, overcommit })) {
                bsd_error(cpu,
                    priority >= DarwinPthreadRuntime::workqueue_priority_count
                        ? bsd_support::invalid_argument
                        : darwin::error::no_memory);
                return true;
            }
            if (!service_bsd_workqueue(cpu)) {
                bsd_error(cpu, darwin::error::no_memory);
                return true;
            }
            bsd_success(cpu, 0);
            return true;
        }
        if (operation == queue_remove) {
            if (priority >= DarwinPthreadRuntime::workqueue_priority_count) {
                bsd_error(cpu, bsd_support::invalid_argument);
            } else if (!pthread_runtime_.remove_workitem(
                           item_address, priority)) {
                bsd_error(cpu, darwin::error::no_such_process);
            } else {
                bsd_success(cpu, 0);
            }
            return true;
        }
        if (operation == thread_set_concurrency) {
            if (!pthread_runtime_.set_target_concurrency(priority, affinity)) {
                bsd_error(cpu, bsd_support::invalid_argument);
                return true;
            }
            if (!service_bsd_workqueue(cpu)) {
                bsd_error(cpu, darwin::error::no_memory);
                return true;
            }
            bsd_success(cpu, 0);
            return true;
        }
        if (operation == thread_return) {
            const auto processor =
                static_cast<std::uint32_t>(cpu.processor_id());
            const auto worker = pthread_runtime_.worker(processor);
            if (!worker) {
                bsd_error(cpu, bsd_support::invalid_argument);
                return true;
            }
            const auto next_item = pthread_runtime_.take_workitem();
            if (next_item) {
                const auto& registration = *pthread_runtime_.registration();
                cpu.registers() = workqueue_thread_state(
                    registration, *worker, *next_item, true);
                cpu.set_cpsr(
                    pthread_start_cpsr(registration.workqueue_thread_start));
                pthread_runtime_.mark_worker_running(
                    processor, next_item->priority);
                output_.write(
                    "[pthread] workqueue continue pid=" +
                    std::to_string(process_.pid) +
                    " slot=" + std::to_string(processor) +
                    " item=" + std::to_string(next_item->address) +
                    " priority=" + std::to_string(next_item->priority) + "\n");
                return true;
            }
            pthread_runtime_.park_worker(processor);
            process_.waiting_for_events = true;
            output_.write(
                "[pthread] workqueue park pid=" + std::to_string(process_.pid) +
                " slot=" + std::to_string(processor) + "\n");
            cpu.halt(Dynarmic::HaltReason::UserDefined5);
            return true;
        }

        bsd_error(cpu, bsd_support::invalid_argument);
        return true;
    }
    case 372: { // thread_selfid
        const auto thread_object =
            thread_object_for_processor(cpu.processor_id());
        if (!thread_object) {
            bsd_error(cpu, bsd_support::invalid_argument);
            return true;
        }
        bsd_success(cpu, *thread_object, 0);
        return true;
    }
    default:
        return false;
    }
}

} // namespace ilemu
