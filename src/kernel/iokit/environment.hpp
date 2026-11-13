// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// Publish profile-driven environmental sensor registry services.

#pragma once

#include "kernel/kernel_shared_state.hpp"

#include <cstddef>
#include <cstdint>
#include <span>

namespace ilemu {

struct KernelSharedState;

namespace kernel_iokit::environment {

    [[nodiscard]] bool matches_service(
        std::span<const std::byte> matching, const KernelSharedState& state);

    [[nodiscard]] bool matches_hid_service(std::span<const std::byte> matching);

    // The caller holds KernelSharedState::mach_mutex.
    [[nodiscard]] std::uint32_t ensure_hid_service_locked(
        KernelSharedState& state);
    [[nodiscard]] std::uint32_t set_hid_properties_locked(
        KernelSharedState& state, KernelSharedState::IOKitService& service,
        std::span<const std::byte> data);

    // The caller holds KernelSharedState::mach_mutex.
    [[nodiscard]] std::uint32_t ensure_service_locked(KernelSharedState& state);

} // namespace kernel_iokit::environment
} // namespace ilemu
