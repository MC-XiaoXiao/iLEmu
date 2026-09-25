// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#pragma once

#include "kernel/kernel_iokit_graphics.hpp"

namespace ilemu::kernel_iokit::graphics {

// The struct-method ABI is negotiated by IOAccelDevice's initial mapping
// request. Legacy clients share selector numbers but use a different layout.
// The caller owns state.mach_mutex.
[[nodiscard]] std::optional<MethodResult> dispatch_device_method_locked(
    AddressSpace& memory, KernelSharedState& state,
    std::uint32_t connection_object, std::uint32_t selector,
    std::span<const std::uint64_t> scalar_input,
    std::span<const std::byte> inband_input,
    std::uint32_t scalar_output_capacity,
    std::uint32_t inband_output_capacity);

} // namespace ilemu::kernel_iokit::graphics
