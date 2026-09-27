// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "clock/server.hpp"
#include "kernel/kernel_clock.hpp"
#include "mach/clock_reply_mig_ids.hpp"
#include <algorithm>
#include <array>
#include <iterator>

namespace ilemu {
namespace {
    void enqueue_alarm_locked(KernelSharedState& state,
        const KernelSharedState::ClockAlarm& alarm, std::uint32_t code,
        std::uint64_t now)
    {
        using namespace mach_support;
        const auto send = alarm.reply_right == xnu::ipc::Right::Send;
        if (!state.mach_port_objects.contains(alarm.reply_object)) {
            if (send)
                release_inflight_send_right_locked(state, alarm.reply_object);
            return;
        }
        using namespace xnu::mig::clock_reply;
        const std::array<std::uint32_t, 12> words { send ? 17U : 18U, 48U,
            alarm.reply_object, 0U, 0U, id(Routine::clock_alarm_reply), 0U, 1U,
            code, alarm.alarm_type,
            static_cast<std::uint32_t>(
                now / darwin::mach::clock::nanoseconds_per_second),
            static_cast<std::uint32_t>(
                now % darwin::mach::clock::nanoseconds_per_second) };
        KernelSharedState::MachMessage message;
        message.bytes.resize(sizeof(words));
        for (std::size_t i = 0; i < words.size(); ++i)
            write_little_word(
                message.bytes, i * sizeof(std::uint32_t), words[i]);
        message.destination = alarm.reply_object;
        if (send)
            message.destination_send_object = alarm.reply_object;
        state.enqueue_mach_message_locked(
            alarm.reply_object, std::move(message));
    }
    void schedule_alarm_locked(KernelSharedState& state,
        const KernelSharedState::ClockAlarm& alarm, std::uint32_t seconds,
        std::uint32_t nanos)
    {
        using namespace darwin::mach;
        const auto now = state.clock.now();
        const auto valid = alarm.alarm_type <= clock::time_relative &&
                           nanos < clock::nanoseconds_per_second;
        const auto now_seconds =
            static_cast<std::uint32_t>(now / clock::nanoseconds_per_second);
        const auto now_nanos =
            static_cast<std::uint32_t>(now % clock::nanoseconds_per_second);
        if (valid && alarm.alarm_type == clock::time_relative) {
            // ADD_MACH_TIMESPEC carries nanoseconds and wraps unsigned tv_sec.
            nanos += now_nanos;
            if (nanos >= clock::nanoseconds_per_second) {
                nanos -= clock::nanoseconds_per_second;
                ++seconds;
            }
            seconds += now_seconds;
        }
        if (!valid || seconds < now_seconds ||
            (seconds == now_seconds && nanos <= now_nanos)) {
            enqueue_alarm_locked(
                state, alarm, valid ? success : invalid_value, now);
        } else {
            const auto delay =
                (static_cast<std::uint64_t>(seconds) - now_seconds) *
                    clock::nanoseconds_per_second +
                nanos - now_nanos;
            state.clock_alarms.emplace(now + delay, alarm);
            state.note_kernel_event_transition();
        }
    }
} // namespace

std::uint32_t kernel_clock::Server::register_alarm_locked(
    KernelSharedState& state, std::uint32_t clock_id,
    KernelSharedState::MachMessage& request)
{
    using namespace mach_support;
    using namespace darwin::mach;
    const auto& bytes = request.bytes;
    if (bytes.size() != 60U ||
        (read_little_word(bytes, 0U) & darwin::mig_wire::message_complex_bit) ==
            0 ||
        read_little_word(bytes, 24U) != 1U)
        return darwin::mig::bad_arguments;
    if ((read_little_word(bytes, 36U) >> 24U) != 0U)
        return darwin::mig::type_error;
    // The calendar clock has no alarm operation in the reference XNU range.
    if (clock_id != clock::system_clock_id)
        return failure;
    const auto transfer = std::find_if(request.port_transfers.begin(),
        request.port_transfers.end(), [](const auto& port) {
            return port.descriptor_offset == 28U && !port.array_index;
        });
    if (transfer == request.port_transfers.end() ||
        (transfer->right != xnu::ipc::Right::Send &&
            transfer->right != xnu::ipc::Right::SendOnce))
        return invalid_capability;
    const KernelSharedState::ClockAlarm alarm { read_little_word(bytes, 48U),
        transfer->object, transfer->right };
    request.port_transfers.erase(transfer);
    schedule_alarm_locked(state, alarm, read_little_word(bytes, 52U),
        read_little_word(bytes, 56U));
    return success;
}

std::optional<std::uint32_t> kernel_clock::Server::try_alarm_synchronous_locked(
    AddressSpace& memory, KernelSharedState& state,
    const ProcessContext& process, std::uint32_t clock_id,
    std::span<const std::uint32_t> registers, std::uint32_t bits,
    std::uint32_t reply_name, std::uint32_t receive_address)
{
    using namespace mach_support;
    constexpr auto rpc_options = darwin::mach_message::option_send |
                                 darwin::mach_message::option_receive |
                                 darwin::mach_message::option_receive_timeout |
                                 darwin::mach_message::option_receive_large;
    if (clock_id != darwin::mach::clock::system_clock_id ||
        bits != 0x80001513U || (registers[1] & 3U) != 3U ||
        (registers[1] & ~rpc_options) != 0U || registers[2] != 60U ||
        registers[3] < 44U || registers[4] != reply_name)
        return std::nullopt;
    const auto reply = resolve_name_with_right(
        state, process.pid, reply_name, xnu::ipc::Right::Receive);
    if (!reply || !state.mach_port_objects.contains(*reply) ||
        state.mach_port_set_links_by_member.contains(*reply))
        return std::nullopt;
    const auto queue = state.mach_queues.find(*reply);
    if (queue == state.mach_queues.end() || !queue->second.empty())
        return std::nullopt;
    std::array<std::byte, 60> bytes;
    if (!memory.copy_out(registers[0], bytes))
        return darwin::mach_message::send_invalid_data;
    if (read_little_word(bytes, 24U) != 1U ||
        (read_little_word(bytes, 36U) >> 16U) != 21U)
        return std::nullopt;
    const auto alarm_port = resolve_name_with_right(state, process.pid,
        read_little_word(bytes, 28U), xnu::ipc::Right::Receive);
    if (!alarm_port || *alarm_port == *reply ||
        !state.mach_port_objects.contains(*alarm_port))
        return std::nullopt;
    // COPY_SEND/MAKE_SEND_ONCE have no lasting namespace mutations. Both
    // receive rights belong to this task; the caller excludes waiting peers.
    schedule_alarm_locked(state,
        KernelSharedState::ClockAlarm { read_little_word(bytes, 48U),
            *alarm_port, xnu::ipc::Right::SendOnce },
        read_little_word(bytes, 52U), read_little_word(bytes, 56U));
    constexpr std::array<std::uint32_t, 3> payload { 0U, 1U, 0U };
    return mach_ipc::copyout_simple_kernel_reply_locked(memory, state,
        receive_address, reply_name, *reply, read_little_word(bytes, 20U),
        payload);
}

std::optional<std::uint64_t> next_clock_alarm_deadline_locked(
    const KernelSharedState& state)
{
    if (state.clock_alarms.empty())
        return std::nullopt;
    return state.clock_alarms.begin()->first;
}

void deliver_due_clock_alarms_locked(
    KernelSharedState& state, std::uint64_t deadline)
{
    auto end = state.clock_alarms.upper_bound(deadline);
    if (end == state.clock_alarms.begin())
        return;
    // XNU prepends expired alarms to alrmdone, then drains that list. The
    // timestamp is the sampled expiration time, not the requested deadline.
    const auto now = state.clock.now();
    while (end != state.clock_alarms.begin()) {
        const auto alarm = std::prev(end);
        const auto value = alarm->second;
        end = state.clock_alarms.erase(alarm);
        enqueue_alarm_locked(state, value, darwin::mach::success, now);
    }
    state.note_kernel_event_transition();
}

void cancel_clock_alarms_locked(
    KernelSharedState& state, std::uint32_t reply_object)
{
    bool changed = false;
    for (auto alarm = state.clock_alarms.begin();
        alarm != state.clock_alarms.end();) {
        if (alarm->second.reply_object != reply_object) {
            ++alarm;
            continue;
        }
        const auto send = alarm->second.reply_right == xnu::ipc::Right::Send;
        alarm = state.clock_alarms.erase(alarm);
        if (send)
            mach_support::release_inflight_send_right_locked(
                state, reply_object);
        changed = true;
    }
    if (changed)
        state.note_kernel_event_transition();
}
} // namespace ilemu
