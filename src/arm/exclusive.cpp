/* SPDX-License-Identifier: MPL-2.0 */
#include "exclusive.hpp"
namespace ilemu::arm {
bool decode_exclusive(Instruction& out, std::uint32_t word, bool thumb) noexcept
{
    if (word == (thumb ? 0xf3bf8f2fU : 0xf57ff01fU)) {
        out.kind = InstructionKind::ClearExclusive;
        if (!thumb)
            out.condition = 14;
        return true;
    }
    if (!thumb) {
        if (out.condition == 15 || (word & 0x0f8000f0U) != 0x01800090U)
            return false;
        if ((word & 0xf00U) != 0xf00U)
            return true;
        constexpr unsigned widths[] = { 4, 8, 1, 2 };
        out.access_size = widths[(word >> 21U) & 3U];
        out.load = (word & (1U << 20U)) != 0;
        out.rn = (word >> 16U) & 15U;
        out.rd = (word >> 12U) & 15U;
        out.rm = word & 15U;
        if (out.load && out.rm != 15)
            return true;
        const auto first = out.load ? out.rd : out.rm;
        out.rs = first + 1U;
        if (out.access_size == 8 && ((first & 1U) != 0 || out.rs == 15))
            return true;
    } else {
        const auto head = word >> 16U;
        const auto tail = word & 0xffffU;
        if ((head & 0xffe0U) == 0xe840U) {
            out.load = (head & 16U) != 0;
            if (out.load && (tail & 0xf00U) != 0xf00U)
                return true;
            out.access_size = 4;
            out.immediate = (tail & 255U) << 2U;
            out.rd = out.load ? tail >> 12U : (tail >> 8U) & 15U;
            out.rm = tail >> 12U;
        } else if ((head & 0xffe0U) == 0xe8c0U) {
            const auto operation = (tail >> 4U) & 15U;
            if (operation != 4 && operation != 5 && operation != 7)
                return false; // other synchronization/table-branch families
            out.load = (head & 16U) != 0;
            out.access_size = operation == 7 ? 8U : operation == 5 ? 2U : 1U;
            if ((out.load && (tail & 15U) != 15) ||
                (out.access_size != 8 && (tail & 0xf00U) != 0xf00U))
                return true;
            out.rd = out.load ? tail >> 12U : tail & 15U;
            out.rm = tail >> 12U;
            out.rs = (tail >> 8U) & 15U;
        } else
            return false;
        out.rn = head & 15U;
        const auto first = out.load ? out.rd : out.rm;
        // ARMv7 Thumb forbids SP in data/status operands, but permits an SP
        // base. STREXD permits equal source registers; LDREXD does not.
        if (first == 13 || (!out.load && out.rd == 13) ||
            (out.access_size == 8 &&
                (out.rs == 13 || out.rs == 15 || (out.load && first == out.rs))))
            return true;
    }
    const auto first = out.load ? out.rd : out.rm;
    if (out.rn == 15 || first == 15 ||
        (!out.load && (out.rd == 15 || out.rd == out.rn || out.rd == first ||
            (out.access_size == 8 && out.rd == out.rs))))
        return true;
    out.kind = InstructionKind::ExclusiveTransfer;
    return true;
}
}
