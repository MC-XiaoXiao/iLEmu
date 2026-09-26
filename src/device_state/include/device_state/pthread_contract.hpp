// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#pragma once

#include <cstdint>

#include "device_state/darwin_abi.hpp"

namespace ilemu {

enum class PthreadRegistrationLayout { Unavailable, Arm32RegisterV1, RegisterV2 };
enum class PthreadPriorityEncoding { QueueIndex, FineClassBitsV2 };

// Resolved ABI properties, not scheduling policy. The catalog instances are
// immutable and live for the host process. V2 registration remains unsupported;
// its independent thread identity query is still available.
struct PthreadContract {
    PthreadRegistrationLayout registration_layout;
    std::uint32_t thread_pointer_offset;
    PthreadPriorityEncoding priority_encoding;
    std::uint32_t workqueue_priority_count;
    std::uint32_t registration_features;
    bool supports_thread_identity;
    bool supports_bsdthread_ctl;

    [[nodiscard]] constexpr bool supports_registration_v1() const noexcept
    {
        return registration_layout == PthreadRegistrationLayout::Arm32RegisterV1;
    }
};

// Resolve at configuration initialization, never in syscall/worker hot paths.
// Invalid enum values are configuration errors, not a legacy ABI fallback.
[[nodiscard]] const PthreadContract& resolve_pthread_contract(DarwinPthreadAbi abi);

} // namespace ilemu
