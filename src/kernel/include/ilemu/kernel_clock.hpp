#pragma once

#include <cstdint>
#include <optional>

namespace ilemu {

struct KernelSharedState;
struct ProcessContext;
class AddressSpace;

[[nodiscard]] std::optional<std::uint32_t> handle_clock_mach_request(
    AddressSpace& memory, KernelSharedState& state, ProcessContext& process,
    std::uint32_t message_id, std::uint32_t message_address,
    std::uint32_t send_size, std::uint32_t receive_size,
    std::uint32_t remote_port, std::uint32_t local_port);

// The caller holds KernelSharedState::mach_mutex for every locked operation.
void enqueue_clock_alarm_reply_locked(KernelSharedState& state,
    std::uint32_t reply_object, std::uint32_t code, std::uint32_t alarm_type,
    std::uint64_t alarm_time);

[[nodiscard]] std::optional<std::uint64_t> next_clock_alarm_deadline_locked(
    const KernelSharedState& state);

void deliver_due_clock_alarms_locked(
    KernelSharedState& state, std::uint64_t deadline);

} // namespace ilemu
