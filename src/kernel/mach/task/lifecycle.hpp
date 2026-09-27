// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#pragma once
#include "suspension.hpp"
#include "../transport/kernel_reply.hpp"
#include "mach/task_mig_ids.hpp"

namespace ilemu::task_mig {
class Lifecycle {
public:
    static bool handles(std::uint32_t identifier)
    {
        using xnu::mig::task::Routine;
        return identifier == static_cast<std::uint32_t>(Routine::task_suspend) ||
               identifier == static_cast<std::uint32_t>(Routine::task_resume);
    }

    static std::optional<std::uint32_t> dispatch_locked(KernelSharedState& state,
        std::uint32_t caller, std::uint32_t object,
        KernelSharedState::MachMessage& request, bool& suspended)
    {
        using namespace mach_support;
        const auto identifier = read_little_word(request.bytes, 20U);
        const bool resume = identifier == static_cast<std::uint32_t>(xnu::mig::task::Routine::task_resume);
        auto result = darwin::mig::bad_arguments;
        if (request.bytes.size() == 24U &&
            (read_little_word(request.bytes, 0U) & darwin::mig_wire::message_complex_bit) == 0U) {
            const auto target = state.task_port_pids.find(object);
            result = target == state.task_port_pids.end() ? darwin::mach::invalid_argument
                : Suspension::change_locked(state, caller, target->second, resume);
        }
        suspended = !resume && result == darwin::mach::success;
        const std::array<std::uint32_t, 3> payload { 0U, 1U, result };
        return mach_ipc::enqueue_kernel_reply_locked(state, request, identifier, payload);
    }
};
} // namespace ilemu::task_mig
