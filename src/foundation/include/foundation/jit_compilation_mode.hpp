// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>

namespace ilemu {

// Host compilation policy, independent of guest device, image and firmware.
enum class JitCompilationMode : std::uint8_t { Optimized, Baseline };

// Keep ordinary blocks intact and bound work on unusually large cold blocks.
[[nodiscard]] constexpr std::size_t jit_compilation_block_limit(
    JitCompilationMode mode) noexcept
{
    return mode == JitCompilationMode::Baseline ? 64U : 1024U;
}

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
