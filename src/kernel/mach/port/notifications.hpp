// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#pragma once

#include "foundation/address_space.hpp"
#include "kernel/kernel_shared_state.hpp"
#include "mach/mach_port_mig_ids.hpp"

namespace ilemu::port_mig {
class Notifications {
public:
    struct SynchronousReply {
        std::optional<std::uint32_t> received_status;
    };

    static std::optional<SynchronousReply> try_synchronous_locked(AddressSpace& memory,
        KernelSharedState& state, const ProcessContext& process,
        std::span<const std::uint32_t> registers, std::uint32_t bits,
        std::uint32_t reply_name, std::uint32_t receive_address);

    static bool handles(std::uint32_t identifier)
    {
        using namespace xnu::mig::mach_port;
        return identifier == id(Routine::mach_port_request_notification);
    }

    static std::optional<std::uint32_t> dispatch_locked(
        KernelSharedState& state, std::uint32_t object,
        KernelSharedState::MachMessage& request);

private:
    struct Result {
        std::uint32_t error { };
        std::uint32_t previous { };
    };

    static Result evaluate_locked(KernelSharedState& state,
        std::uint32_t object, KernelSharedState::MachMessage& request);
    static std::optional<std::uint32_t> reply_locked(KernelSharedState& state,
        KernelSharedState::MachMessage& request, Result result);

    static Result register_locked(KernelSharedState& state,
        std::uint32_t object, std::uint32_t name, std::uint32_t kind,
        std::uint32_t sync, std::uint32_t notify);
};
} // namespace ilemu::port_mig
