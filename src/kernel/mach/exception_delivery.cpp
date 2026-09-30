// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
// XNU osfmk/kern/exception.c and osfmk/mach/{exc,mach_exc}.defs.
#include "foundation/cpu.hpp"
#include "kernel/darwin_signal_context.hpp"
#include "kernel/kernel_mach_task_identity.hpp"
#include "kernel/kernel_shared_state.hpp"
#include "kernel/mach_arm_thread_abi.hpp"
#include "kernel/mach_exception_delivery.hpp"
#include "mach/mig_wire_abi.hpp"
#include "support.hpp"
#include <algorithm>
#include <mutex>
#include <span>
#include <vector>

namespace ilemu {
using namespace mach_support;
namespace {
    constexpr std::uint32_t wide_codes = 0x80000000U;
    constexpr std::uint32_t exception_default = 1, exception_state = 2;
}

void MachExceptionDelivery::retire_reply_locked(
    KernelSharedState& state, Pending& pending)
{
    if (pending.reply_object)
        terminate_receive_object_locked(state, pending.reply_object);
    pending.reply_object = 0;
}

bool MachExceptionDelivery::advance_locked(KernelSharedState& state,
    const ProcessContext& process, Cpu& cpu, Pending& pending)
{
    const auto task = mach_task_identity::control_port_locked(state, process);
    const auto threads = state.task_thread_port_objects.find(process.pid);
    if (threads == state.task_thread_port_objects.end())
        return false;
    const auto found =
        threads->second.find(static_cast<std::uint32_t>(cpu.processor_id()));
    if (found == threads->second.end())
        return false;
    const auto thread = found->second;
    while (pending.next_level < 3U) {
        const auto level = pending.next_level++;
        KernelSharedState::TaskExceptionAction action;
        if (level == 2U)
            action = state.host_exception_actions[pending.exception.type];
        else if (const auto actions = state.exception_port_actions.find(
                     level == 0U ? thread : task);
            actions != state.exception_port_actions.end())
            action = actions->second[pending.exception.type];
        if (!action.port_object ||
            !state.mach_port_objects.contains(action.port_object))
            continue;
        const auto behavior = action.behavior & ~wide_codes;
        const bool identity = behavior != exception_state;
        const bool with_state = behavior != exception_default;
        if (behavior < 1U || behavior > 3U)
            continue;
        // Match the thread_get/set_state flavors implemented by this kernel.
        // Invalid state flavors fail this action and continue triage in XNU.
        if (with_state &&
            action.flavor != darwin::arm_thread::general_state_flavor)
            continue;
        const bool wide = (action.behavior & wide_codes) != 0;
        const auto identifier = (wide ? 2404U : 2400U) + behavior;
        const auto ndr = identity ? 52U : 24U;
        const auto codes = ndr + 16U;
        const auto state_header = codes + (wide ? 16U : 8U);
        const auto size =
            state_header +
            (with_state ? 8U + 4U * darwin::arm_thread::general_state_word_count
                        : 0U);
        KernelSharedState::MachMessage message;
        message.bytes.resize(size);
        const auto write = [&](std::size_t offset, std::uint32_t value) {
            write_little_word(message.bytes, offset, value);
        };
        pending.reply_object = state.allocate_mach_object();
        // This private receive capability belongs to the task continuation,
        // has no guest name, and routes replies through the ordinary queue.
        if (!state.mach_port_objects.create(
                pending.reply_object, process.pid)) {
            pending.reply_object = 0;
            continue;
        }
        pending.reply_identifier = identifier + 100U;
        pending.behavior = behavior;
        write(0, 17U | (18U << 8U) | (identity ? 0x80000000U : 0U));
        write(4, static_cast<std::uint32_t>(size));
        write(8, action.port_object);
        write(12, pending.reply_object);
        write(20, identifier);
        if (identity) {
            write(24, 2);
            for (const auto& [offset, object] :
                { std::pair { 28U, thread }, std::pair { 40U, task } }) {
                write(offset, object);
                write(offset + 8U, 17U << 16U);
                message.port_transfers.push_back({ offset, 0, std::nullopt,
                    object, xnu::ipc::Right::Send, 17U });
                ++state.mach_inflight_send_rights[object];
            }
        }
        write(ndr + 4U, 1);
        write(ndr + 8U, pending.exception.type);
        write(ndr + 12U, 2);
        for (std::size_t i = 0; i < 2U; ++i) {
            const auto offset = codes + i * (wide ? 8U : 4U);
            write(
                offset, static_cast<std::uint32_t>(pending.exception.codes[i]));
            if (wide)
                write(offset + 4U, static_cast<std::uint32_t>(
                                       pending.exception.codes[i] >> 32U));
        }
        if (with_state) {
            write(state_header, action.flavor);
            write(state_header + 4U,
                darwin::arm_thread::general_state_word_count);
            for (std::size_t i = 0; i < cpu.registers().size(); ++i)
                write(state_header + 8U + i * 4U, cpu.registers()[i]);
            write(state_header + 8U + darwin::arm_thread::cpsr_index * 4U,
                cpu.cpsr());
        }
        message.destination = action.port_object;
        message.destination_send_object = action.port_object;
        ++state.mach_inflight_send_rights[action.port_object];
        message.reply_object = pending.reply_object;
        message.reply_right = xnu::ipc::Right::SendOnce;
        state.mach_port_objects.make_send_once(pending.reply_object);
        // Kernel RPC uses MACH_SEND_ALWAYS; it does not park on a user qlimit.
        state.enqueue_mach_message_locked(
            action.port_object, std::move(message));
        return true;
    }
    return false;
}

MachExceptionDelivery::Completion MachExceptionDelivery::begin(
    KernelSharedState& state, const ProcessContext& process, Cpu& cpu,
    Exception exception)
{
    const std::lock_guard lock { state.mach_mutex };
    auto& pending = pending_[cpu.processor_id()];
    retire_reply_locked(state, pending);
    pending = Pending { exception };
    if (advance_locked(state, process, cpu, pending))
        return { Outcome::Waiting, exception };
    pending_.erase(cpu.processor_id());
    return { Outcome::Unhandled, exception };
}

MachExceptionDelivery::Completion MachExceptionDelivery::poll(
    KernelSharedState& state, const ProcessContext& process, Cpu& cpu)
{
    const std::lock_guard lock { state.mach_mutex };
    auto& pending = pending_.at(cpu.processor_id());
    const auto exception = pending.exception;
    auto queue = state.mach_queues.find(pending.reply_object);
    if (state.mach_port_objects.contains(pending.reply_object) &&
        (queue == state.mach_queues.end() || queue->second.empty()))
        return { Outcome::Waiting, exception };
    bool handled = !state.mach_port_objects.contains(pending.reply_object);
    if (!handled) {
        auto reply = std::move(queue->second.front());
        queue->second.pop_front();
        state.note_mach_message_dequeued_locked(pending.reply_object);
        const auto word = [&](std::size_t offset) {
            return read_little_word(reply.bytes, offset);
        };
        const auto size = reply.bytes.size();
        const bool valid =
            size >= 36U && word(4) == size && !(word(0) & 0x80000000U) &&
            word(20) == pending.reply_identifier && word(32) == 0U;
        if (valid && pending.behavior == exception_default)
            handled = size == 36U;
        else if (valid && size >= 44U &&
                 word(36) == darwin::arm_thread::general_state_flavor &&
                 word(40) >= darwin::arm_thread::general_state_word_count &&
                 word(40) <= 144U && size == 44U + word(40) * 4U) {
            for (std::size_t i = 0; i < cpu.registers().size(); ++i)
                cpu.registers()[i] = word(44U + i * 4U);
            cpu.set_cpsr(darwin::signal_context::restored_cpsr(
                word(44U + darwin::arm_thread::cpsr_index * 4U), cpu.cpsr()));
            handled = true;
        }
        if (reply.destination_send_once_object) {
            state.mach_port_objects.release_send_once(
                *reply.destination_send_once_object);
            reply.destination_send_once_object.reset();
        }
        discard_mach_message_rights_locked(state, reply);
    }
    retire_reply_locked(state, pending);
    if (!handled && advance_locked(state, process, cpu, pending))
        return { Outcome::Waiting, exception };
    pending_.erase(cpu.processor_id());
    return { handled ? Outcome::Handled : Outcome::Unhandled, exception };
}

bool MachExceptionDelivery::ready(
    KernelSharedState& state, std::size_t slot) const
{
    const auto it = pending_.find(slot);
    if (it == pending_.end())
        return false;
    const std::lock_guard lock { state.mach_mutex };
    const auto queue = state.mach_queues.find(it->second.reply_object);
    return !state.mach_port_objects.contains(it->second.reply_object) ||
           (queue != state.mach_queues.end() && !queue->second.empty());
}
void MachExceptionDelivery::cancel(KernelSharedState& state, std::size_t slot)
{
    const auto it = pending_.find(slot);
    if (it == pending_.end())
        return;
    const std::lock_guard lock { state.mach_mutex };
    retire_reply_locked(state, it->second);
    pending_.erase(it);
}
void MachExceptionDelivery::clear(KernelSharedState& state)
{
    const std::lock_guard lock { state.mach_mutex };
    for (auto& [slot, pending] : pending_)
        retire_reply_locked(state, pending);
    pending_.clear();
}
} // namespace ilemu
