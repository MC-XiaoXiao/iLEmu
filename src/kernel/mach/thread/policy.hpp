// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#pragma once

#include "../transport/kernel_reply.hpp"
#include "kernel/mach_thread_policy_abi.hpp"
#include "mach/xnu_scheduler.hpp"

namespace ilemu::thread_mig {
class Policy {
public:
    static bool handles(std::uint32_t identifier)
    {
        using namespace darwin::mach::thread_policy;
        return identifier == policy_set_message ||
               identifier == policy_get_message;
    }

    template <typename Apply, typename Query>
    static std::optional<std::uint32_t> dispatch_locked(
        KernelSharedState& state, std::uint32_t object,
        KernelSharedState::MachMessage& request, const Apply& apply,
        const Query& query)
    {
        using namespace mach_support;
        using namespace darwin::mach::thread_policy;
        const auto identifier = read_little_word(request.bytes, 20U);
        const auto error = [&](std::uint32_t result) {
            const std::array<std::uint32_t, 3> payload { 0U, 1U, result };
            return mach_ipc::enqueue_kernel_reply_locked(
                state, request, identifier, payload);
        };
        const bool get = identifier == policy_get_message;
        if ((read_little_word(request.bytes, 0U) &
                darwin::mig_wire::message_complex_bit) != 0U ||
            request.bytes.size() < 40U)
            return error(darwin::mig::bad_arguments);
        const auto flavor =
            read_little_word(request.bytes, request_flavor_offset);
        const auto count =
            read_little_word(request.bytes, request_count_offset);
        if ((get && request.bytes.size() != 44U) ||
            (!get && (count > maximum_policy_word_count ||
                         request.bytes.size() != 40U + count * 4U)))
            return error(darwin::mig::bad_arguments);
        const auto owner = find_thread_owner(state, object);
        if (!owner)
            return error(darwin::mach::invalid_argument);
        const auto statistics =
            query ? query(owner->first, owner->second) : std::nullopt;
        if (!statistics)
            return error(darwin::mach::terminated);
        if (!get) {
            std::array<std::uint32_t, maximum_policy_word_count> values { };
            for (std::uint32_t index = 0; index < count; ++index)
                values[index] =
                    read_little_word(request.bytes, 40U + index * 4U);
            return error(apply && apply(owner->first, owner->second, flavor,
                                      std::span { values }.first(count))
                             ? darwin::mach::success
                             : darwin::mach::invalid_argument);
        }

        // MIG CountInOut is clamped to the wire maximum, not to the flavor.
        // These XNU flavors leave count unchanged; initialize excess words.
        const auto output_count =
            std::min<std::uint32_t>(count, maximum_policy_word_count);
        auto get_default = read_little_word(request.bytes, 40U);
        std::array<std::uint32_t, maximum_policy_word_count + 5U> payload { };
        payload[1] = 1U;
        payload[3] = output_count;
        const auto& policy = statistics->policy;
        switch (flavor) {
        case extended_policy:
            if (!get_default && policy.realtime_mode)
                get_default = 1U;
            if (output_count >= extended_policy_word_count)
                payload[4] = get_default || policy.timeshare;
            break;
        case time_constraint_policy: {
            if (output_count < time_constraint_policy_word_count)
                return error(darwin::mach::invalid_argument);
            if (!get_default && !policy.realtime_mode)
                get_default = 1U;
            auto realtime = policy.realtime;
            if (get_default) {
                const auto quantum = static_cast<std::uint32_t>(
                    statistics->quantum_ticks * absolute_time_units_per_second /
                    statistics->ticks_per_second);
                realtime = { 0U, quantum / 2U, quantum, 1U };
            }
            payload[4] = realtime.period;
            payload[5] = realtime.computation;
            payload[6] = realtime.constraint;
            payload[7] = realtime.preemptible;
            break;
        }
        case precedence_policy:
            if (output_count < precedence_policy_word_count)
                return error(darwin::mach::invalid_argument);
            payload[4] = get_default
                             ? 0U
                             : static_cast<std::uint32_t>(policy.importance);
            break;
        default:
            return error(darwin::mach::invalid_argument);
        }
        payload[4U + output_count] = get_default;
        return mach_ipc::enqueue_kernel_reply_locked(state, request, identifier,
            std::span { payload }.first(5U + output_count));
    }
};
} // namespace ilemu::thread_mig
