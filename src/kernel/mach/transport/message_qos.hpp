/* SPDX-License-Identifier: MPL-2.0 */
#pragma once

#include "kernel/darwin_abi.hpp"
#include "kernel/kernel_shared_state.hpp"
#include "../../bsd/pthread_priority.hpp"

namespace ilemu::mach_transport {

// Caller holds mach_mutex. Voucher attributes are immutable and the copied-in
// right keeps them alive even if the sender subsequently drops its name.
inline MachMessageQos copyin_message_qos(const KernelSharedState& state,
    const KernelSharedState::MachMessage& message, std::uint32_t options,
    std::uint32_t override_priority)
{
    MachMessageQos result;
    const auto& contract = state.pthread_contract;
    if (contract.priority_encoding == PthreadPriorityEncoding::QueueIndex)
        return result;
    const auto tier = [&](std::uint32_t encoded) {
        const auto decoded =
            PthreadPriority::decode_dispatch_workqueue_priority(
                contract, encoded);
        return decoded
                   ? PthreadPriority::qos_tier_for_class(decoded->thread_class)
                   : XnuQosTier::Unspecified;
    };
    if (message.voucher_object) {
        const auto voucher = state.mach_vouchers.find(*message.voucher_object);
        if (voucher != state.mach_vouchers.end()) {
            if (const auto priority = voucher->second.pthread_priority())
                result.requested = tier(*priority);
        }
    }
    result.override_tier = result.requested;
    if ((options & darwin::mach_message::option_send_override) != 0U)
        result.override_tier =
            std::max(result.override_tier, tier(override_priority));
    return result;
}

} // namespace ilemu::mach_transport
