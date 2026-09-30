// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
#pragma once
#include "../transport/kernel_reply.hpp"
#include "mach/vm_map_mig_ids.hpp"
namespace ilemu::vm_mig {
class Deallocation {
public:
    static bool handles(std::uint32_t identifier)
    {
        return identifier == xnu::mig::vm_map::id(
                   xnu::mig::vm_map::Routine::vm_deallocate) || identifier == 4801U;
    }
    static std::uint32_t execute(AddressSpace& memory, Cpu& cpu,
        std::uint64_t address, std::uint64_t size, bool wide);
    static std::optional<std::uint32_t> dispatch_locked(AddressSpace& memory,
        Cpu& cpu, KernelSharedState& state, std::uint32_t caller,
        std::uint32_t object, KernelSharedState::MachMessage& request);
    static std::optional<std::uint32_t> try_synchronous_locked(AddressSpace& memory,
        Cpu& cpu, KernelSharedState& state, const ProcessContext& process,
        std::span<const std::uint32_t> registers, std::uint32_t bits,
        std::uint32_t reply_name, std::uint32_t receive_address, std::uint32_t identifier);
private:
    static std::uint32_t evaluate_locked(AddressSpace& memory, Cpu& cpu,
        const KernelSharedState& state, std::uint32_t caller,
        std::uint32_t object, std::span<const std::byte> bytes);
};
} // namespace ilemu::vm_mig
