// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// Darwin process-private user-lock wait and wake operations.
// Apple reference: xnu-3789.1.32/bsd/kern/sys_ulock.c.

#include "kernel/kernel.hpp"

#include "kernel/darwin_abi.hpp"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <optional>
#include <vector>

#include "../mach/support.hpp"
#include "support.hpp"

namespace ilemu {
namespace {

constexpr std::uint32_t compare_and_wait = 1U;
constexpr std::uint32_t unfair_lock = 2U;
constexpr std::uint32_t opcode_mask = 0xffU;
constexpr std::uint32_t wake_all = 0x100U;
constexpr std::uint32_t wake_thread = 0x200U;
constexpr std::uint32_t workqueue_contention = 0x10000U;
constexpr std::uint32_t no_errno = 0x1000000U;

} // namespace

void CompatibilityKernel::promote_ulock_owner(UlockState& lock,
    std::optional<std::size_t> owner, std::size_t donor)
{
    const auto old_owner = lock.owner;
    if (owner && thread_user_promotion_handler_) {
        thread_user_promotion_handler_(*owner, donor, lock.promotion,
            old_owner == owner ? XnuUserPromotionAction::Update
                               : XnuUserPromotionAction::Add);
    }
    lock.owner = owner;
    if (!owner)
        lock.promotion = {};
    if (old_owner && old_owner != owner && thread_user_promotion_handler_)
        thread_user_promotion_handler_(*old_owner, donor, lock.promotion,
            XnuUserPromotionAction::Drop);
}

std::size_t CompatibilityKernel::finish_ulock_wait(std::size_t processor)
{
    const auto pending = pending_ulock_waits_.find(processor);
    auto lock = ulocks_.find(pending->second.address);
    pending_ulock_waits_.erase(pending);
    const auto remaining = --lock->second.waiters;
    if (remaining == 0) {
        promote_ulock_owner(lock->second, std::nullopt, processor);
        ulocks_.erase(lock);
    }
    return remaining;
}

void CompatibilityKernel::retire_ulock_thread(std::size_t processor)
{
    if (pending_ulock_waits_.contains(processor))
        finish_ulock_wait(processor);
    for (auto& [address, lock] : ulocks_) {
        if (lock.owner == processor)
            promote_ulock_owner(lock, std::nullopt, processor);
    }
}

void CompatibilityKernel::clear_ulock_waits()
{
    for (auto& [address, lock] : ulocks_)
        promote_ulock_owner(lock, std::nullopt, 0);
    ulocks_.clear();
    pending_ulock_waits_.clear();
}

void CompatibilityKernel::dispatch_bsd_ulock(Cpu& cpu, std::uint32_t number)
{
    const auto& registers = cpu.registers();
    const auto operation = registers[0];
    const auto opcode = operation & opcode_mask;
    const auto flags = operation & ~opcode_mask;
    const auto address = registers[1];
    const bool waiting = number == 515U;
    const auto permitted_flags = waiting
        ? no_errno | workqueue_contention
        : no_errno | wake_all | wake_thread;
    const auto fail = [&](std::uint32_t error) {
        if ((flags & no_errno) != 0U)
            bsd_success(cpu, 0U - error);
        else
            bsd_error(cpu, error);
    };

    if ((flags & ~permitted_flags) != 0U ||
        (opcode != compare_and_wait && opcode != unfair_lock) ||
        address == 0U || (address & 3U) != 0U ||
        (!waiting && (flags & (wake_all | wake_thread)) ==
                         (wake_all | wake_thread))) {
        fail(bsd_support::invalid_argument);
        return;
    }

    if (waiting) {
        if (const auto lock = ulocks_.find(address);
            lock != ulocks_.end() && lock->second.opcode != opcode) {
            fail(33U); // EDOM.
            return;
        }
        const auto value = memory_.read32(address);
        if (!value) {
            fail(bsd_support::bad_address);
            return;
        }
        if (*value != registers[2]) {
            bsd_success(cpu, 0);
            return;
        }
        std::optional<std::size_t> owner;
        if (opcode == unfair_lock) {
            // os_unfair_lock stores the owning thread's Mach port name with
            // its low state bits cleared. XNU restores IPC's name mask.
            const auto name = registers[2] | 3U;
            {
                std::lock_guard mach_lock { shared_state_->mach_mutex };
                const auto object = mach_support::resolve_name_with_right(
                    *shared_state_, process_.pid, name, xnu::ipc::Right::Send);
                if (object) {
                    const auto thread = mach_support::find_thread_owner(
                        *shared_state_, *object);
                    if (thread && thread->first == process_.pid &&
                        thread->second != cpu.processor_id())
                        owner = thread->second;
                }
            }
            // XNU permits MACH_PORT_DEAD for the pthread no-TSD lock.
            if (!owner && name != std::numeric_limits<std::uint32_t>::max()) {
                fail(105U); // EOWNERDEAD.
                return;
            }
        }
        std::optional<std::uint64_t> deadline;
        const auto timeout = registers[4]; // ARM32's aligned uint64 value occupies r2:r3.
        if (timeout != 0U) {
            constexpr std::uint64_t nanoseconds_per_microsecond = 1000U;
            const auto now = shared_state_->clock.now();
            const auto interval = static_cast<std::uint64_t>(timeout) *
                nanoseconds_per_microsecond;
            deadline = interval > std::numeric_limits<std::uint64_t>::max() - now
                ? std::numeric_limits<std::uint64_t>::max()
                : now + interval;
        }
        auto& lock = ulocks_.try_emplace(address, UlockState { opcode, 0, std::nullopt, {} }).first->second;
        ++lock.waiters;
        if (opcode == unfair_lock)
            promote_ulock_owner(lock, owner, cpu.processor_id());
        pending_ulock_waits_.insert_or_assign(cpu.processor_id(),
            PendingUlockWait { address, opcode, flags,
                next_ulock_wait_sequence_++, deadline, false });
        if (owner)
            scheduler_handoffs_.insert_or_assign(cpu.processor_id(),
                XnuThreadId { process_.pid, static_cast<std::uint32_t>(*owner) });
        process_.waiting_for_events = true;
        cpu.halt(Dynarmic::HaltReason::UserDefined5);
        return;
    }

    std::optional<std::size_t> target;
    if ((flags & wake_thread) != 0U) {
        std::lock_guard mach_lock { shared_state_->mach_mutex };
        const auto object = mach_support::resolve_name_with_right(
            *shared_state_, process_.pid, registers[2], xnu::ipc::Right::Send);
        if (object) {
            const auto owner = mach_support::find_thread_owner(
                *shared_state_, *object);
            if (owner && owner->first == process_.pid)
                target = owner->second;
        }
        if (!target) {
            fail(darwin::error::no_such_process);
            return;
        }
    }

    const auto lock = ulocks_.find(address);
    if (lock == ulocks_.end()) {
        fail(2U); // ENOENT.
        return;
    }
    if (lock->second.opcode != opcode) {
        fail(33U); // EDOM.
        return;
    }

    std::vector<WokenThread> woken;
    std::uint64_t first_sequence = std::numeric_limits<std::uint64_t>::max();
    std::int32_t highest_priority = -1;
    std::int32_t highest_scheduled_priority = -1;
    std::optional<std::size_t> first_processor;
    for (auto& [processor, pending] : pending_ulock_waits_) {
        if (pending.address != address || pending.awakened)
            continue;
        if (pending.opcode != opcode || (target && processor != *target))
            continue;
        if ((flags & wake_all) != 0U) {
            pending.awakened = true;
            woken.push_back({ process_.pid, processor });
        } else {
            const auto statistics = thread_statistics_query_
                ? thread_statistics_query_(process_.pid, static_cast<std::uint32_t>(processor)) : std::nullopt;
            const auto priority = statistics
                ? statistics->scheduling.effective_base_priority
                : xnu::scheduler::default_base_priority;
            const auto scheduled = statistics
                ? statistics->scheduling.scheduled_priority : priority;
            if (priority > highest_priority ||
                (priority == highest_priority &&
                    (scheduled > highest_scheduled_priority ||
                        (scheduled == highest_scheduled_priority &&
                            pending.sequence < first_sequence)))) {
                highest_priority = priority;
                highest_scheduled_priority = scheduled;
                first_sequence = pending.sequence;
                first_processor = processor;
            }
        }
    }
    if (first_processor) {
        pending_ulock_waits_.at(*first_processor).awakened = true;
        woken.push_back({ process_.pid, *first_processor });
    }
    // A different thread may already have acquired and been promoted for
    // this lock. An earlier owner's wake must preserve that newer donation.
    if (lock->second.owner == cpu.processor_id())
        promote_ulock_owner(lock->second, std::nullopt, cpu.processor_id());
    if (target && woken.empty()) {
        fail(37U); // EALREADY.
        return;
    }
    wake_threads_and_maybe_preempt(cpu, woken);
    bsd_success(cpu, 0);
}

} // namespace ilemu
