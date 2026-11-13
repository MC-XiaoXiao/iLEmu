/* SPDX-License-Identifier: MPL-2.0 */
#pragma once
#include "foundation/output.hpp"
#include "foundation/performance.hpp"
#include "kernel/kernel_shared_state.hpp"
#include "mach/mig_wire_abi.hpp"
#include <chrono>
#include <mutex>
#include <sstream>
#include <string_view>

namespace ilemu::mach_transport {
inline std::uint64_t flow_timestamp()
{
    return static_cast<std::uint64_t>(std::chrono::duration_cast<
        std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
}

inline bool flow_for_process(KernelSharedState& state, std::uint32_t pid)
{
    if (!performance_counters().cpu_source_diagnostics_enabled())
        return false;
    const std::lock_guard lock { state.mach_mutex };
    return state.foreground_application_attempt_process_id == pid;
}

// Called under mach_mutex. Trace only traffic to/from the current physical
// launch target during an explicitly enabled CPU diagnostic window.
inline void trace_foreground_message(KernelSharedState& state, Output& output,
    const KernelSharedState::MachMessage& message, std::uint32_t actor,
    std::size_t processor, std::uint32_t options, std::string_view phase)
{
    if (!performance_counters().cpu_source_diagnostics_enabled())
        return;
    const auto foreground = state.foreground_application_attempt_process_id;
    if (!foreground)
        return;
    const auto port = state.mach_port_objects.lookup(message.destination);
    const auto receiver = phase == "send" ? (port ? port->receive_owner : 0U) : actor;
    if (message.sender_pid != *foreground && receiver != *foreground)
        return;
    const auto word = [&](std::size_t offset) {
        std::uint32_t value = 0;
        if (offset + 4U <= message.bytes.size())
            for (std::size_t i = 0; i < 4U; ++i)
                value |= std::to_integer<std::uint32_t>(message.bytes[offset + i]) << (8U * i);
        return value;
    };
    std::ostringstream trace;
    trace << "[perf] ipc-flow phase=" << phase << " time-ns=" << flow_timestamp()
          << " stamp=" << message.host_enqueue_nanoseconds
          << " actor=" << actor << " cpu=" << processor
          << " source=" << message.sender_pid << " receiver=" << receiver
          << " destination=" << message.destination
          << " reply=" << message.reply_object.value_or(0U)
          << " id=" << word(20U) << " bytes=" << message.bytes.size()
          << " options=" << options
          << " requested=" << static_cast<unsigned>(message.qos.requested)
          << " override=" << static_cast<unsigned>(message.qos.override_tier)
          << " voucher=" << message.voucher_object.value_or(0U)
          << " importance=" << static_cast<bool>(message.importance)
          << " donating=" << (message.importance && message.importance->donating())
          << " port-owner=" << (port ? port->receive_owner : 0U)
          << " port-importance=" << (port && port->importance_receiver)
          << " temporary=" << (port && port->temporary_owner)
          << " transit=" << (port ? port->transit_destination : 0U)
          << " assertions=" << (port ? port->importance->assertions() : 0U);
    const auto policy = [&](std::string_view label, std::uint32_t pid) {
        const auto process = state.processes.find(pid);
        if (process == state.processes.end())
            return;
        const auto& value = *process->second.execution_policy;
        trace << ' ' << label << "-type=" << static_cast<unsigned>(value.type())
              << ' ' << label << "-role=" << static_cast<int>(value.role())
              << ' ' << label << "-background=" << value.qos_policy().background
              << ' ' << label << "-donating=" << value.importance()->donating()
              << ' ' << label << "-boosted=" << value.importance()->boosted();
    };
    policy("source", message.sender_pid);
    policy("receiver", receiver);
    output.line(trace.str());
}
} // namespace ilemu::mach_transport
