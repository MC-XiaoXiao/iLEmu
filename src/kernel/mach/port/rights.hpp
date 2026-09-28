// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
#pragma once
#include <kernel/kernel_shared_state.hpp>
#include <foundation/address_space.hpp>

namespace ilemu::port_mig {
class Rights {
public:
    struct SynchronousReply {
        std::optional<std::uint32_t> received_status;
    };
    static std::optional<SynchronousReply> try_synchronous_locked(
        AddressSpace& memory, KernelSharedState& state,
        const ProcessContext& process, std::span<const std::uint32_t> registers,
        std::uint32_t bits, std::uint32_t reply_name,
        std::uint32_t receive_address);
    static bool handles(std::uint32_t identifier);
    static std::optional<std::uint32_t> dispatch_locked(
        KernelSharedState& state, std::uint32_t object,
        KernelSharedState::MachMessage& request);
    static std::uint32_t insert_locked(KernelSharedState& state,
        std::uint32_t caller, std::uint32_t target, std::uint32_t name,
        std::uint32_t source, std::uint32_t disposition);
private:
    using Transfer = KernelSharedState::MachMessage::PortTransfer;
    struct Copyin {
        std::uint32_t error { };
        std::optional<Transfer> right;
    };
    static std::optional<std::uint32_t> reply_extracted_locked(
        KernelSharedState& state, KernelSharedState::MachMessage& request,
        std::uint32_t identifier, const Transfer& token);
    static Copyin copyin_locked(KernelSharedState& state, std::uint32_t task,
        std::uint32_t name, std::uint32_t disposition);
    static std::uint32_t insert_object_locked(KernelSharedState& state,
        std::uint32_t task, std::uint32_t name, const Transfer& right);
    static void release_locked(KernelSharedState& state, const Transfer& right,
        bool installed);
};
} // namespace ilemu::port_mig
