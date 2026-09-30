// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
#pragma once
#include "../transport/kernel_reply.hpp"
#include "mach/vm_map_mig_ids.hpp"
namespace ilemu::vm_mig {
class Allocation {
public:
    static bool handles(std::uint32_t identifier)
    {
        return identifier == xnu::mig::vm_map::id(
                   xnu::mig::vm_map::Routine::vm_allocate) || identifier == 4800U;
    }
    static std::optional<std::uint32_t> dispatch_locked(AddressSpace& memory,
        KernelSharedState& state, std::uint32_t caller, std::uint32_t object,
        KernelSharedState::MachMessage& request);
    static std::optional<std::uint32_t> try_synchronous_locked(AddressSpace& memory,
        KernelSharedState& state, const ProcessContext& process,
        std::span<const std::uint32_t> registers, std::uint32_t bits,
        std::uint32_t reply_name, std::uint32_t receive_address, std::uint32_t identifier);
private:
    struct Result {
        std::array<std::uint32_t, 5> words { 0U, 1U };
        std::uint32_t address_words = 1U;
        explicit Result(std::uint32_t error) { words[2] = error; }
        std::span<const std::uint32_t> payload() const
        {
            return std::span { words }.first(words[2] == 0U ? 3U + address_words : 3U);
        }
    };
    static Result evaluate_locked(AddressSpace& memory, const KernelSharedState& state,
        std::uint32_t caller, std::uint32_t object, std::span<const std::byte> bytes);
};
} // namespace ilemu::vm_mig
