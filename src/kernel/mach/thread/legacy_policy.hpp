// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#pragma once

#include "../transport/kernel_reply.hpp"
#include "kernel/mach_thread_policy_abi.hpp"
#include "mach/xnu_scheduler.hpp"

namespace ilemu::thread_mig {
class LegacyPolicy {
public:
    template <typename Apply>
    static std::optional<std::uint32_t> dispatch_locked(
        KernelSharedState& state, std::uint32_t object,
        KernelSharedState::MachMessage& request, const Apply& apply)
    {
        using namespace mach_support;
        using namespace darwin::mach::thread_policy;
        const auto reply = [&](std::uint32_t result) {
            const std::array<std::uint32_t, 3> payload { 0U, 1U, result };
            return mach_ipc::enqueue_kernel_reply_locked(
                state, request, legacy_policy_message, payload);
        };
        if ((read_little_word(request.bytes, 0U) &
                darwin::mig_wire::message_complex_bit) != 0U ||
            request.bytes.size() < 44U)
            return reply(darwin::mig::bad_arguments);
        const auto count =
            read_little_word(request.bytes, legacy_request_count_offset);
        if (count > legacy_maximum_policy_word_count ||
            request.bytes.size() != 44U + count * 4U)
            return reply(darwin::mig::bad_arguments);
        const auto owner = find_thread_owner(state, object);
        if (!owner)
            return reply(darwin::mach::invalid_argument);
        const auto policy =
            read_little_word(request.bytes, legacy_request_policy_offset);
        if (policy != legacy_timeshare_policy &&
            policy != legacy_round_robin_policy && policy != legacy_fifo_policy)
            return reply(28U); // KERN_INVALID_POLICY
        const auto expected = policy == legacy_round_robin_policy ? 2U : 1U;
        if (count != expected)
            return reply(darwin::mach::invalid_argument);
        const auto priority = std::bit_cast<std::int32_t>(
            read_little_word(request.bytes, legacy_request_base_offset));
        const auto set_limit =
            read_little_word(request.bytes, 40U + count * 4U);
        // mk_sp.c checks the existing limit before validating priority.
        if (!set_limit && priority > xnu::scheduler::maximum_user_priority)
            return reply(27U); // KERN_POLICY_LIMIT
        if (priority < xnu::scheduler::minimum_priority ||
            priority > xnu::scheduler::maximum_priority)
            return reply(darwin::mach::invalid_argument);
        // RR quantum and set_limit do not alter the native quantum or max.
        return reply(
            apply && apply(owner->first, owner->second, policy, priority)
                ? darwin::mach::success
                : darwin::mach::invalid_argument);
    }
};
} // namespace ilemu::thread_mig
