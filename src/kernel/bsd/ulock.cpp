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
        const auto value = memory_.read32(address);
        if (!value) {
            fail(bsd_support::bad_address);
            return;
        }
        if (*value != registers[2]) {
            bsd_success(cpu, 0);
            return;
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
        pending_ulock_waits_.insert_or_assign(cpu.processor_id(),
            PendingUlockWait { address, opcode, flags,
                next_ulock_wait_sequence_++, deadline, false });
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

    std::vector<WokenThread> woken;
    std::uint64_t first_sequence = std::numeric_limits<std::uint64_t>::max();
    std::optional<std::size_t> first_processor;
    bool address_has_waiter = false;
    for (auto& [processor, pending] : pending_ulock_waits_) {
        if (pending.address != address || pending.awakened)
            continue;
        address_has_waiter = true;
        if (pending.opcode != opcode || (target && processor != *target))
            continue;
        if ((flags & wake_all) != 0U) {
            pending.awakened = true;
            woken.push_back({ process_.pid, processor });
        } else if (pending.sequence < first_sequence) {
            first_sequence = pending.sequence;
            first_processor = processor;
        }
    }
    if (first_processor) {
        pending_ulock_waits_.at(*first_processor).awakened = true;
        woken.push_back({ process_.pid, *first_processor });
    }
    if (woken.empty()) {
        fail(address_has_waiter ? 33U : 2U); // EDOM or ENOENT.
        return;
    }
    wake_threads_and_maybe_preempt(cpu, woken);
    bsd_success(cpu, 0);
}

} // namespace ilemu
