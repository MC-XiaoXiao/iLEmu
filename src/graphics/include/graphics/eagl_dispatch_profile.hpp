// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// Recognize the ARM32 EAGL context and dispatch-table calling
// convention.

#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

namespace ilemu {

// The ARM32 GLI ABI keeps an opaque context followed by a function table.
// Resolve slots from the firmware's public wrappers instead of assuming that
// an entry has the same index across driver revisions.
class EaglContextFirstArm32Profile {
public:
    static constexpr std::uint32_t dispatch_bytes = 0xe24U;
    static constexpr std::uint32_t context_offset = 0x10U;
    static constexpr std::uint32_t front_dispatch_offset = 0x14U;

    [[nodiscard]] static std::optional<std::uint32_t> dispatch_slot(
        std::span<const std::byte> thumb_wrapper);
};

} // namespace ilemu
