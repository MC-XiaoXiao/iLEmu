// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
#pragma once

#include <foundation/address_space.hpp>
#include <kernel/kernel_shared_state.hpp>
#include <array>

namespace ilemu::port_mig {
class Context {
public:
    // Extensions after the base mach_port adapter's last routine.
    static constexpr std::uint32_t get_identifier = 3228U;
    static constexpr std::uint32_t set_identifier = 3229U;
    static bool handles(std::uint32_t identifier)
    {
        return identifier == get_identifier || identifier == set_identifier;
    }
    static std::optional<std::uint32_t> dispatch_locked(
        KernelSharedState& state, std::uint32_t object,
        KernelSharedState::MachMessage& request);
    static std::optional<std::uint32_t> try_synchronous_locked(
        AddressSpace& memory, KernelSharedState& state,
        const ProcessContext& process, std::span<const std::uint32_t> registers,
        std::uint32_t bits, std::uint32_t reply_name,
        std::uint32_t receive_address);

private:
    struct Result {
        std::array<std::uint32_t, 5> words { 0U, 1U };
        std::size_t count { 3U };
        explicit Result(std::uint32_t error = 0U) { words[2] = error; }
        std::span<const std::uint32_t> payload() const
        {
            return std::span { words }.first(count);
        }
    };
    static Result evaluate_locked(KernelSharedState& state,
        std::uint32_t object, std::span<const std::byte> bytes);
};
} // namespace ilemu::port_mig
