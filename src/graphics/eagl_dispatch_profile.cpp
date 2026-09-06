// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// Recognize the ARM32 EAGL context and dispatch-table calling
// convention.

#include "graphics/eagl_dispatch_profile.hpp"

#include <array>

namespace ilemu {

std::optional<std::uint32_t> EaglContextFirstArm32Profile::dispatch_slot(
    std::span<const std::byte> code)
{
    const auto halfword = [&](std::size_t offset) {
        return std::to_integer<std::uint32_t>(code[offset]) |
               (std::to_integer<std::uint32_t>(code[offset + 1U]) << 8U);
    };
    std::array<bool, 16> private_context { };
    std::array<std::optional<std::uint32_t>, 16> function_offsets { };
    std::optional<std::uint32_t> tls_register;
    bool passes_context = false;
    for (std::size_t offset = 0; offset + 2U <= code.size();) {
        const auto first = halfword(offset);
        const bool wide = (first & 0xf800U) >= 0xe800U;
        if (wide && offset + 4U > code.size())
            return std::nullopt;
        const auto second = wide ? halfword(offset + 2U) : 0U;
        std::optional<std::uint32_t> base;
        std::uint32_t destination { };
        std::uint32_t immediate { };
        if (wide && first == 0xee1dU && (second & 0x0fffU) == 0x0f70U) {
            tls_register = second >> 12U; // MRC p15, 0, Rt, c13, c0, 3
        } else if (!wide && (first & 0xf800U) == 0x6800U) {
            base = (first >> 3U) & 7U;
            destination = first & 7U;
            immediate = ((first >> 6U) & 31U) * 4U;
        } else if (wide && (first & 0xfff0U) == 0xf8d0U) {
            base = first & 15U;
            destination = second >> 12U;
            immediate = second & 0xfffU;
        } else if (!wide && (first & 0xff87U) == 0x4780U) {
            const auto target = (first >> 3U) & 15U;
            if (passes_context && function_offsets[target])
                return (*function_offsets[target] - front_dispatch_offset) / 4U;
            return std::nullopt;
        }
        if (!tls_register && (offset >= 24U || first == 0x4770U ||
                (first & 0xff00U) == 0xbd00U ||
                (!wide && (first & 0xf800U) == 0xe000U)))
            return std::nullopt;
        if (base) {
            const bool from_private = private_context[*base];
            function_offsets[destination].reset();
            private_context[destination] = tls_register == base && immediate == 0x78U;
            if (from_private) {
                if (destination == 0U && immediate == context_offset)
                    passes_context = true;
                else if (immediate >= front_dispatch_offset &&
                         immediate - front_dispatch_offset < dispatch_bytes &&
                         (immediate & 3U) == 0U)
                    function_offsets[destination] = immediate;
            }
        }
        offset += wide ? 4U : 2U;
    }
    return std::nullopt;
}

} // namespace ilemu
