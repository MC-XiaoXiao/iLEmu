// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "foundation/cpu.hpp"
#include "foundation/userland_hle.hpp"

#include <stdexcept>
#include <utility>

namespace ilemu {

void UserlandHleCall::suspend_until(std::function<bool()> ready,
    Continuation completion, std::uint64_t completion_sequence)
{
    if (!ready || !completion || pending_ready_ || tail_call_address_ ||
        resume_original_ || resume_original_persistently_)
        throw std::logic_error { "invalid pending HLE continuation" };
    pending_ready_ = std::move(ready);
    pending_completion_ = std::move(completion);
    pending_sequence_ = completion_sequence;
}

void UserlandHleRegistry::park_call(UserlandHleCall& call)
{
    std::array<std::uint32_t, 16> arguments;
    for (std::size_t i = 0; i < arguments.size(); ++i)
        arguments[i] = call.argument(i);
    const auto slot = call.cpu_.processor_id();
    pending_host_calls_.insert_or_assign(
        slot, PendingHostCall { call.process_id_, process_incarnation_,
                  mapping_generation_, host_call_thread_generations_[slot],
                  call.cpu_.registers(), arguments, call.cpu_.cpsr(),
                  std::string { call.symbol_ }, call.prefix_argument_count_,
                  call.entry_address_, call.registration_id_,
                  call.pending_sequence_, std::move(call.pending_ready_),
                  std::move(call.pending_completion_) });
    // Even an already completed ticket parks once. The next serial kernel
    // boundary observes its retained level state, so publication cannot race
    // the scheduler's subsequent Block transition and lose a wakeup.
    call.cpu_.halt(Dynarmic::HaltReason::UserDefined5);
}

bool UserlandHleRegistry::pending_call(std::size_t processor) const
{
    return pending_host_calls_.contains(processor);
}

bool UserlandHleRegistry::pending_call_ready(std::size_t processor) const
{
    const auto pending = pending_host_calls_.find(processor);
    return pending != pending_host_calls_.end() && pending->second.ready();
}

std::optional<std::uint64_t> UserlandHleRegistry::pending_call_sequence(
    std::size_t processor) const
{
    const auto found = pending_host_calls_.find(processor);
    if (found == pending_host_calls_.end())
        return std::nullopt;
    return found->second.sequence;
}

bool UserlandHleRegistry::complete_pending_call(
    Cpu& cpu, std::uint32_t process_id)
{
    const auto slot = cpu.processor_id();
    const auto found = pending_host_calls_.find(slot);
    if (found == pending_host_calls_.end() || !found->second.ready())
        return false;
    auto pending = std::move(found->second);
    pending_host_calls_.erase(found); // one consumer, before any continuation
    if (pending.process_id != process_id ||
        pending.incarnation != process_incarnation_ ||
        pending.mapping_generation != mapping_generation_ ||
        pending.thread_generation != host_call_thread_generations_[slot])
        return false;
    cpu.registers() = pending.registers;
    cpu.set_cpsr(pending.cpsr);
    cpu.clear_halt();
    UserlandHleCall call { *this, cpu, memory_, output_, process_id,
        pending.symbol, pending.prefix_arguments };
    call.saved_arguments_ = pending.arguments;
    call.entry_address_ = pending.entry_address;
    call.registration_id_ = pending.registration_id;
    pending.completion(call);
    const bool completed = !call.pending_ready_;
    if (!finish_call(call))
        throw std::runtime_error { "cannot resume pending HLE return path" };
    return completed;
}

void UserlandHleRegistry::register_call_gate(
    std::string suffix, std::function<void(UserlandHleCall&, Handler)> gate)
{
    call_gates_.insert_or_assign(std::move(suffix), std::move(gate));
}

void UserlandHleRegistry::cancel_pending_call(std::size_t processor)
{
    pending_host_calls_.erase(processor);
    ++host_call_thread_generations_[processor];
}

void UserlandHleRegistry::cancel_pending_calls()
{
    pending_host_calls_.clear();
    host_call_thread_generations_.clear();
    ++mapping_generation_;
}

} // namespace ilemu
