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
        using xnu::mig::task::id;
        return identifier == id(Routine::task_suspend) ||
               identifier == id(Routine::task_resume) ||
               identifier == id(Routine::task_suspend2) ||
               identifier == id(Routine::task_resume2);
    }

    static std::optional<std::uint32_t> dispatch_locked(KernelSharedState& state,
        std::uint32_t caller, std::uint32_t object,
        KernelSharedState::MachMessage& request, bool& suspended)
    {
        using namespace mach_support;
        using xnu::mig::task::Routine;
        using xnu::mig::task::id;
        const auto identifier = read_little_word(request.bytes, 20U);
        const bool suspend_token = identifier == id(Routine::task_suspend2);
        const bool resume_token = identifier == id(Routine::task_resume2);
        std::uint32_t token = 0;
        const bool resume = resume_token || identifier == id(Routine::task_resume);
        auto result = darwin::mig::bad_arguments;
        if ((suspend_token || resume_token) &&
            state.darwin_abi.task_suspension !=
                DarwinTaskSuspensionAbi::ResumePortTokens) {
            result = darwin::mig::bad_id;
        } else if (request.bytes.size() == 24U &&
            (read_little_word(request.bytes, 0U) & darwin::mig_wire::message_complex_bit) == 0U) {
            if (resume_token) {
                result = Suspension::resume_token_locked(state, object);
            } else {
                const auto target = state.task_port_pids.find(object);
                result = target == state.task_port_pids.end() ? darwin::mach::invalid_argument
                    : Suspension::change_locked(state, caller, target->second, resume,
                          suspend_token ? Suspension::Hold::Token : Suspension::Hold::Legacy);
                if (suspend_token && result == darwin::mach::success)
                    token = state.processes.at(target->second).task_resume_port;
            }
        }
        suspended = !resume && result == darwin::mach::success;
        if (token != 0) {
            const std::array<std::uint32_t, 4> payload { 1U, token, 0U, 18U << 16U };
            const std::array<KernelSharedState::MachMessage::PortTransfer, 1> ports { {
                { .descriptor_offset = 28U, .sender_name = 0U,
                    .array_index = std::nullopt, .object = token,
                    .right = xnu::ipc::Right::SendOnce, .disposition = 18U }
            } };
            return mach_ipc::enqueue_kernel_reply_locked(
                state, request, identifier, payload, ports);
        }
        const std::array<std::uint32_t, 3> payload { 0U, 1U, result };
        return mach_ipc::enqueue_kernel_reply_locked(state, request, identifier, payload);
    }
};
} // namespace ilemu::task_mig
