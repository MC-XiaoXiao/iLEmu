// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
#pragma once

#include <cstdint>
#include <optional>
#include <string_view>

namespace ilemu {

// Host compilation policy, independent of guest device, image and firmware.
enum class JitCompilationMode : std::uint8_t { Optimized, Baseline };

[[nodiscard]] constexpr std::string_view jit_compilation_mode_name(
    JitCompilationMode mode) noexcept
{
    return mode == JitCompilationMode::Baseline ? "baseline" : "optimized";
}

[[nodiscard]] constexpr std::optional<JitCompilationMode>
parse_jit_compilation_mode(std::string_view name) noexcept
{
    if (name == "optimized")
        return JitCompilationMode::Optimized;
    if (name == "baseline")
        return JitCompilationMode::Baseline;
    return std::nullopt;
}

} // namespace ilemu
