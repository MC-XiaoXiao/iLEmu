/* SPDX-License-Identifier: MPL-2.0 */
// XNU3789 bsd/kern/stackshot.c and kern/telemetry.c. The ARM32 firmware
// wrapper passes a 44-byte config: its two uint64 output pointers start at28/36.
#include "kernel/kernel.hpp"
#include "kernel/darwin_abi.hpp"
#include "../../mach/support.hpp"
#include <algorithm>

namespace ilemu {
namespace {
std::uint64_t nanoseconds(std::uint64_t ticks, std::uint32_t frequency)
{
    return static_cast<std::uint64_t>(
        static_cast<__uint128_t>(ticks) * 1'000'000'000U / frequency);
}
} // namespace

diagnostic_snapshot::Task CompatibilityKernel::capture_diagnostic_task(std::uint32_t pid)
{
    diagnostic_snapshot::Task result { };
    {
        const std::lock_guard lock { shared_state_->mach_mutex };
        const auto& record = shared_state_->processes.at(pid);
        result.pid = pid;
        result.unique_id = record.incarnation;
        // This snapshot exports no kernel UUID or shared-region loadinfo.
        result.flags = 0xa00000U | (record.pid_suspended ? 0x10U : 0U) |
            (record.dirty_self ? 0x10000U : 0U);
        result.start_wall_ns = record.start_wall_nanoseconds;
        result.suspend_count = record.task_user_stop_count + record.task_legacy_stop_count;
        result.name = record.command;
    }
    if (task_memory_statistics_query_) {
        if (const auto memory = task_memory_statistics_query_(pid)) {
            result.resident_bytes = memory->resident_bytes;
            result.peak_resident_bytes = memory->maximum_resident_bytes;
        }
    }
    if (task_statistics_query_) {
        if (const auto statistics = task_statistics_query_(pid, false))
            result.terminated_user_ns = nanoseconds(
                statistics->terminated_user_ticks, statistics->ticks_per_second);
    }
    return result;
}

diagnostic_snapshot::Thread CompatibilityKernel::capture_diagnostic_thread(
    std::uint32_t pid, std::uint32_t processor, std::uint32_t object,
    std::span<const std::uint32_t> registers)
{
    diagnostic_snapshot::Thread result { };
    result.id = object; // same identity exported by thread_selfid
    result.state = 4U;
    if (thread_statistics_query_) {
        if (const auto statistics = thread_statistics_query_(pid, processor)) {
            result.user_ns = nanoseconds(statistics->user_ticks, statistics->ticks_per_second);
            const auto& scheduling = statistics->scheduling;
            result.state = scheduling.state == XnuThreadState::Waiting ? 1U : 4U;
            result.suspended = statistics->suspend_count != 0U;
            if (result.suspended)
                result.state |= 2U;
            if (statistics->uninterruptible)
                result.state |= 8U;
            result.base_priority = scheduling.base_priority;
            result.scheduled_priority = scheduling.scheduled_priority;
            result.requested_qos = static_cast<std::uint8_t>(scheduling.requested_qos.tier);
        }
    }
    result.program_counters.push_back(registers[15]);
    if (registers[14] != 0U && registers[14] != registers[15])
        result.program_counters.push_back(registers[14]);
    return result;
}

void CompatibilityKernel::sample_microstackshot(Cpu& cpu, std::uint64_t)
{
    const auto object = thread_object_for_processor(cpu.processor_id());
    if (!object)
        return;
    const auto task = capture_diagnostic_task(process_.pid);
    const auto thread = capture_diagnostic_thread(process_.pid,
        static_cast<std::uint32_t>(cpu.processor_id()), *object, cpu.registers());
    shared_state_->microstackshots.record(static_cast<std::uint32_t>(cpu.processor_id()),
        shared_state_->clock.wall_time(), task, thread);
}

void CompatibilityKernel::dispatch_bsd_stackshot(Cpu& cpu, std::uint32_t number)
{
    const auto& r = cpu.registers();
    if (process_.effective_uid != 0U) {
        bsd_error(cpu, darwin::error::operation_not_permitted);
        return;
    }
    if (number == 492U) {
        const auto flags = r[2];
        if ((flags & 0x60U) != 0U) {
            shared_state_->microstackshots.enable((flags & 0x20U) != 0U);
            bsd_success(cpu, 0U);
            return;
        }
        if (r[0] == 0U || r[1] == 0U) {
            bsd_error(cpu, darwin::error::invalid_argument);
            return;
        }
        if ((flags & 0x10U) != 0U) {
            if (r[1] > 8U * 1024U * 1024U) {
                bsd_error(cpu, darwin::error::invalid_argument);
                return;
            }
            if (r[1] < diagnostic_snapshot::Microstackshots::capacity) {
                bsd_error(cpu, darwin::error::no_space_on_device);
                return;
            }
            const auto bytes = shared_state_->microstackshots.read();
            if (!memory_.copy_to_user(r[0], bytes))
                bsd_error(cpu, darwin::error::bad_address);
            else
                bsd_success(cpu, static_cast<std::uint32_t>(bytes.size()));
        } else if ((flags & 0x200U) != 0U) {
            // No bootprofile boot-argument allocated a separate profile buffer.
            if (r[1] > 64U * 1024U * 1024U)
                bsd_error(cpu, darwin::error::invalid_argument);
            else
                bsd_success(cpu, 0U);
        } else {
            bsd_success(cpu, UINT32_MAX);
        }
        return;
    }
    if (r[1] == 0U || (r[0] == 1U && r[2] != 44U)) {
        bsd_error(cpu, darwin::error::invalid_argument);
        return;
    }
    if (r[0] != 1U) {
        bsd_error(cpu, darwin::error::not_supported);
        return;
    }
    const auto config = memory_.read_bytes(r[1], 44U);
    if (!config) {
        bsd_error(cpu, darwin::error::bad_address);
        return;
    }
    const auto field = [&](std::size_t offset) {
        return mach_support::read_little_word(*config, offset);
    };
    const auto pointer = [&](std::size_t offset) {
        return std::uint64_t { field(offset) } | (std::uint64_t { field(offset + 4U) } << 32U);
    };
    const auto flags = field(4U);
    if ((flags & (0x100000U | 0x4000U | 0x400000U)) != 0U) {
        bsd_error(cpu, darwin::error::operation_not_permitted);
        return;
    }
    if ((flags & 0x10000U) == 0U || (flags & 0x40000U) != 0U) {
        bsd_error(cpu, darwin::error::not_supported);
        return;
    }
    const auto buffer_address = pointer(28U), size_address = pointer(36U);
    if (buffer_address == 0U || size_address == 0U || pointer(8U) != 0U) {
        bsd_error(cpu, darwin::error::invalid_argument);
        return;
    }
    if (buffer_address > UINT32_MAX || size_address > UINT32_MAX) {
        bsd_error(cpu, darwin::error::bad_address);
        return;
    }
    if ((flags & (0x8000U | 0x200U)) != 0U) {
        bsd_error(cpu, darwin::error::no_entry);
        return;
    }
    const auto selected_pid = static_cast<std::int32_t>(field(0U));
    std::vector<std::pair<std::uint32_t,
        std::vector<std::pair<std::uint32_t, std::uint32_t>>>> tasks;
    {
        const std::lock_guard lock { shared_state_->mach_mutex };
        for (const auto& [pid, record] : shared_state_->processes) {
            if (record.exited || (selected_pid != -1 && pid != static_cast<std::uint32_t>(selected_pid)))
                continue;
            auto& threads = tasks.emplace_back(pid,
                std::vector<std::pair<std::uint32_t, std::uint32_t>> { }).second;
            if (const auto found = shared_state_->task_thread_port_objects.find(pid);
                found != shared_state_->task_thread_port_objects.end())
                threads.assign(found->second.begin(), found->second.end());
        }
    }
    if (selected_pid != -1 && tasks.empty()) {
        bsd_error(cpu, darwin::error::no_such_process);
        return;
    }
    diagnostic_snapshot::KcData output { shared_state_->clock.now() };
    for (const auto& [pid, threads] : tasks) {
        const auto task = capture_diagnostic_task(pid);
        output.task(task);
        for (const auto& [processor, object] : threads) {
            std::array<std::uint32_t, 17> state;
            if (thread_state_query_ && thread_state_query_(pid, processor, 1U, state))
                output.thread(capture_diagnostic_thread(pid, processor, object, state));
        }
        output.end(task.unique_id);
    }
    const auto bytes = output.finish();
    if (bytes.size() > 8U * 1024U * 1024U) {
        bsd_error(cpu, darwin::error::no_memory);
        return;
    }
    const auto allocation = mach_support::allocate_guest_vm_region(memory_,
        darwin::mach::vm_allocation::Contract { shared_state_->darwin_version },
        0U, bytes.size(), 1U);
    if (allocation.result != darwin::mach::success) {
        bsd_error(cpu, darwin::error::no_space_on_device);
        return;
    }
    std::array<std::byte, 8> pointer_value { };
    mach_support::write_little_word(pointer_value, 0U, allocation.address);
    std::array<std::byte, 4> size_value;
    mach_support::write_little_word(size_value, 0U, static_cast<std::uint32_t>(bytes.size()));
    if (!memory_.copy_to_user(allocation.address, bytes) ||
        !memory_.copy_to_user(static_cast<std::uint32_t>(buffer_address), pointer_value) ||
        !memory_.copy_to_user(static_cast<std::uint32_t>(size_address), size_value)) {
        static_cast<void>(memory_.unmap(allocation.address,
            static_cast<std::uint32_t>((bytes.size() + AddressSpace::page_size - 1U) &
                ~(AddressSpace::page_size - 1U))));
        bsd_error(cpu, darwin::error::bad_address);
        return;
    }
    bsd_success(cpu, 0U);
}

} // namespace ilemu
