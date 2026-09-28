// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
#pragma once
#include <foundation/address_space.hpp>
#include <kernel/kernel_shared_state.hpp>
#include <mach/mach_port_mig_ids.hpp>
#include <array>

namespace ilemu::port_mig {
class Attributes {
public:
    struct SynchronousReply {
        std::optional<std::uint32_t> received_status;
    };
    static bool handles(std::uint32_t identifier)
    {
        using namespace xnu::mig::mach_port;
        return identifier == id(Routine::mach_port_get_attributes) ||
               identifier == id(Routine::mach_port_set_attributes);
    }
    static std::optional<std::uint32_t> dispatch_locked(
        KernelSharedState& state, std::uint32_t object,
        KernelSharedState::MachMessage& request);
    static std::optional<SynchronousReply> try_synchronous_locked(
        AddressSpace& memory, KernelSharedState& state,
        const ProcessContext& process, std::span<const std::uint32_t> registers,
        std::uint32_t bits, std::uint32_t reply_name,
        std::uint32_t receive_address);

private:
    struct Result {
        // NDR, return code, count, then up to ten status words.
        std::array<std::uint32_t, 14> words { 0U, 1U };
        std::size_t count { 3U };
        Result() = default;
        Result(std::uint32_t error) { words[2] = error; }
        std::span<const std::uint32_t> payload() const
        {
            return std::span { words }.first(count);
        }
    };
    static Result evaluate_locked(KernelSharedState& state,
        std::uint32_t object, std::span<const std::byte> bytes);
};
} // namespace ilemu::port_mig
