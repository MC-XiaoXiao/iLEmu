// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#pragma once

#include "foundation/address_space.hpp"
#include "kernel/kernel_shared_state.hpp"
#include "mach/task_mig_ids.hpp"

#include <optional>
#include <span>

namespace ilemu::task_mig {
class Enumeration {
public:
    static bool try_synchronous_enqueue_locked(AddressSpace& memory,
        KernelSharedState& state, const ProcessContext& process,
        std::span<const std::uint32_t> registers, std::uint32_t bits,
        std::uint32_t reply_name);

    static bool handles(std::uint32_t identifier)
    {
        return identifier == xnu::mig::task::id(
            xnu::mig::task::Routine::task_threads);
    }

    static std::optional<std::uint32_t> dispatch_locked(KernelSharedState& state,
        std::uint32_t object, KernelSharedState::MachMessage& request);
};
} // namespace ilemu::task_mig
