#include <foundation/task_vm_events.hpp>
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// Native task_info MIG transport. ARM flavors 4/5 share the narrow layout;
// XNU 792--4903 osfmk/kern/task.c and osfmk/mach/task_info.h.
#include "information.hpp"
#include <algorithm>
#include "cpu_time.hpp"
#include "kernel/task_syscall_counters.hpp"
#include "../transport/kernel_reply.hpp"
#include "kernel/mach_task_info_abi.hpp"

namespace ilemu::task_mig {
using namespace mach_support;
using namespace darwin::mach::task_info;

std::size_t Information::word_count(DarwinTaskInformationAbi abi,
    std::uint32_t flavor, std::uint32_t capacity)
{
    switch (flavor) {
    case absolute_time_flavor: return absolute_time_word_count;
    case events_flavor: return events_word_count;
    case security_token_flavor: return security_token_word_count;
    case audit_token_flavor: return audit_token_word_count;
    case thread_times_flavor: return thread_times_word_count;
    case basic_32_flavor: return basic_32_word_count;
    case basic_64_flavor: return basic_64_word_count;
    case basic_32_peak_flavor:
        return abi >= DarwinTaskInformationAbi::ResidentPeak ? basic_32_peak_word_count : 0U;
    case mach_basic_flavor:
        return abi >= DarwinTaskInformationAbi::UnifiedBasic ? mach_basic_word_count : 0U;
    case dyld_info_flavor:
        if (abi < DarwinTaskInformationAbi::DyldAddressPair)
            return 0U;
        return abi == DarwinTaskInformationAbi::DyldAddressPair || capacity == 4U
            ? 4U : dyld_info_word_count;
    default: return 0U;
    }
}

Information::Result Information::evaluate_locked(AddressSpace& memory,
    KernelSharedState& state, std::uint32_t caller, std::uint32_t object,
    std::span<const std::byte> bytes, const TaskMemoryStatisticsQuery& query, const TaskStatisticsQuery& time_query)
{
    if (bytes.size() != 40U ||
        (read_little_word(bytes, 0U) & darwin::mig_wire::message_complex_bit) != 0U)
        return Result { darwin::mig::bad_arguments };
    const auto target = state.task_port_pids.find(object);
    const auto name_target = state.task_name_port_pids.find(object);
    if (target == state.task_port_pids.end() &&
        name_target == state.task_name_port_pids.end())
        return Result { darwin::mach::invalid_argument };
    const auto pid = target != state.task_port_pids.end()
                         ? target->second : name_target->second;
    const auto process = state.processes.find(pid);
    if (process == state.processes.end() || process->second.exited)
        return Result { darwin::mach::invalid_argument };
    const auto flavor = read_little_word(bytes, 32U);
    const auto capacity = read_little_word(bytes, 36U);
    const auto count = word_count(state.darwin_abi.task_information, flavor, capacity);
    if (count == 0U || capacity < count)
        return Result { darwin::mach::invalid_argument };
    Result result;
    result.words[3] = static_cast<std::uint32_t>(count);
    result.count = 4U + count;
    auto info = std::span { result.words }.subspan(4U, count);
    if (flavor == basic_32_flavor || flavor == basic_64_flavor ||
        flavor == basic_32_peak_flavor || flavor == mach_basic_flavor) {
        const auto resident = pid == caller ? memory.resident_page_statistics()
                                           : AddressSpace::ResidentPageStatistics { };
        const auto statistics = pid == caller
            ? std::optional { TaskMemoryStatistics {
                  static_cast<std::uint64_t>(memory.mapped_page_count()) * AddressSpace::page_size,
                  static_cast<std::uint64_t>(resident.current) * AddressSpace::page_size,
                  static_cast<std::uint64_t>(resident.maximum) * AddressSpace::page_size } }
            : query ? query(pid) : std::nullopt;
        if (!statistics)
            return Result { darwin::mach::invalid_argument };
        if (flavor == mach_basic_flavor) {
            const auto wide = [&info](std::size_t offset, std::uint64_t value) {
                info[offset] = static_cast<std::uint32_t>(value);
                info[offset + 1] = static_cast<std::uint32_t>(value >> 32U);
            };
            wide(0, statistics->virtual_bytes);
            wide(2, statistics->resident_bytes);
            wide(4, statistics->maximum_resident_bytes);
            info[10] = timeshare_policy;
            info[11] = process->second.task_user_stop_count;
        } else {
            info[0] = process->second.task_user_stop_count;
            info[1] = static_cast<std::uint32_t>(statistics->virtual_bytes);
            info[2] = static_cast<std::uint32_t>(flavor == basic_32_peak_flavor
                ? statistics->maximum_resident_bytes : statistics->resident_bytes);
            info[7] = timeshare_policy;
        }
    } else if (flavor == security_token_flavor) {
        info[0] = process->second.effective_uid;
        info[1] = process->second.effective_gid;
    } else if (flavor == audit_token_flavor) {
        const auto token = process->second.audit_token(pid);
        std::copy(token.begin(), token.end(), info.begin());
    } else if (flavor == events_flavor) {
        if (const auto& counters = process->second.vm_events) {
            const auto events = counters->snapshot();
            info[0] = events.faults;
            info[1] = events.pageins;
            info[2] = events.cow_faults;
        }
        if (const auto& counters = process->second.syscall_counters) {
            const auto calls = counters->snapshot();
            info[5] = calls.mach;
            info[6] = calls.unix_calls;
        }
        if (const auto statistics = time_query ? time_query(pid, false) : std::nullopt)
            info[7] = statistics->context_switches;
    } else if (flavor == dyld_info_flavor) {
        // The address may legitimately be zero before dyld publishes it.
        // Accept the pre-format-field count as well as the current count.
        info[0] = process->second.dyld_all_image_info_address;
        info[2] = process->second.dyld_all_image_info_size;
    }
    if (CpuTime::handles(flavor)) {
        const auto statistics = time_query ? time_query(pid,
            flavor == absolute_time_flavor || flavor == thread_times_flavor) : std::nullopt;
        if (statistics)
            CpuTime::write(flavor, *statistics, info);
    }
    return result;
}

std::optional<std::uint32_t> Information::dispatch_locked(AddressSpace& memory,
    KernelSharedState& state, std::uint32_t caller, std::uint32_t object,
    KernelSharedState::MachMessage& request, const TaskMemoryStatisticsQuery& query, const TaskStatisticsQuery& time_query)
{
    const auto result = evaluate_locked(memory, state, caller, object, request.bytes, query, time_query);
    return mach_ipc::enqueue_kernel_reply_locked(state, request, identifier, result.payload());
}

std::optional<std::uint32_t> Information::try_synchronous_locked(AddressSpace& memory,
    KernelSharedState& state, const ProcessContext& process,
    std::span<const std::uint32_t> registers, std::uint32_t bits,
    std::uint32_t reply_name, std::uint32_t receive_address,
    const TaskMemoryStatisticsQuery& query, const TaskStatisticsQuery& time_query)
{
    const auto destination = mach_ipc::validate_task_rpc_locked(memory, state,
        process, registers, bits, reply_name, 40U, 44U);
    if (!destination)
        return std::nullopt;
    std::array<std::byte, 40> bytes;
    if (!memory.copy_out(registers[0], bytes) || read_little_word(bytes, 16U) != 0U)
        return std::nullopt;
    const auto capacity = read_little_word(bytes, 36U);
    const auto count = word_count(state.darwin_abi.task_information, read_little_word(bytes, 32U), capacity);
    const auto size = count != 0U && capacity >= count ? 48U + 4U * count : 44U;
    if (registers[3] < size ||
        !memory.accessible(receive_address, size, MemoryPermission::Write))
        return std::nullopt;
    state.mach_port_objects.make_send_once(destination->reply_object);
    const auto result = evaluate_locked(memory, state, process.pid,
        destination->task_object, bytes, query, time_query);
    return mach_ipc::copyout_kernel_reply_locked<16>(memory, state, receive_address,
        reply_name, destination->reply_object, identifier, result.payload());
}
} // namespace ilemu::task_mig
