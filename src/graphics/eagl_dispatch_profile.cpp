// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// Recognize the ARM32 EAGL context and dispatch-table calling
// convention.

#include "graphics/eagl_dispatch_profile.hpp"

#include <array>

namespace ilemu {

std::optional<EaglContextFirstArm32Profile>
EaglContextFirstArm32Profile::from_dispatch_bytes(std::uint32_t bytes)
{
    if (bytes == 0xe24U || bytes == 0xe48U || bytes == 0x102cU)
        return EaglContextFirstArm32Profile { bytes };
    return std::nullopt;
}

std::optional<std::uint32_t> EaglContextFirstArm32Profile::client_api(
    std::uint32_t flags) const
{
    const bool extended_api = dispatch_bytes_ == 0x102cU;
    const auto api_flags = flags & (extended_api ? 0x78U : 0x0cU);
    if (api_flags == (extended_api ? 0x10U : 0x04U))
        return 1U;
    if (api_flags == (extended_api ? 0x20U : 0x08U))
        return 2U;
    if (extended_api && api_flags == 0x40U)
        return 3U;
    return std::nullopt;
}

std::optional<std::uint32_t> EaglContextFirstArm32Profile::dispatch_slot(
    std::span<const std::byte> code) const
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
        } else if (!wide && (first & 0xff00U) == 0x4600U) {
            // Some wrappers load the dispatch pointer into LR, then move it
            // to IP before restoring LR and tail-calling. Track register
            // copies instead of requiring BX to use the original LDR target.
            const auto source = (first >> 3U) & 15U;
            destination = (first & 7U) | ((first >> 4U) & 8U);
            function_offsets[destination] = function_offsets[source];
            private_context[destination] = private_context[source];
        } else if (!wide && (first & 0xff07U) == 0x4700U) {
            const auto target = (first >> 3U) & 15U;
            if (passes_context && function_offsets[target])
                return (*function_offsets[target] - front_dispatch_offset) / 4U;
            // Leaf wrappers tail-call the driver with BX; their earlier
            // conditional BX LR handles a missing current context.
            if (target != 14U || (first & 0x80U) != 0U)
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
                         immediate - front_dispatch_offset < dispatch_bytes_ &&
                         (immediate & 3U) == 0U)
                    function_offsets[destination] = immediate;
            }
        }
        offset += wide ? 4U : 2U;
    }
    return std::nullopt;
}

} // namespace ilemu
