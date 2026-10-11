/* SPDX-License-Identifier: MPL-2.0 */
#include "arm/floating_comparison.hpp"
#include "floating_context.hpp"
extern "C" {
#include "internals.h"
}
namespace ilemu::arm {
FloatingComparisonResult compare_floating(const FloatingComparisonOperands& v,
    std::uint64_t first, std::uint64_t second, std::uint32_t fpscr) noexcept
{
    floating::Context context { 0 }; // Comparisons do not round.
    std::uint32_t exceptions = 0;
    first = floating::input(v.wide, first, fpscr, exceptions);
    second = v.with_zero ? 0 : floating::input(v.wide, second, fpscr, exceptions);
    bool unordered, equal, less;
    if (v.wide) {
        unordered = isNaNF64UI(first) || isNaNF64UI(second);
        equal = v.signal_all_nans ? f64_eq_signaling({ first }, { second })
                                  : f64_eq({ first }, { second });
        less = !unordered && !equal && f64_lt_quiet({ first }, { second });
    } else {
        const float32_t a { static_cast<std::uint32_t>(first) },
            b { static_cast<std::uint32_t>(second) };
        unordered = isNaNF32UI(a.v) || isNaNF32UI(b.v);
        equal = v.signal_all_nans ? f32_eq_signaling(a, b) : f32_eq(a, b);
        less = !unordered && !equal && f32_lt_quiet(a, b);
    }
    return { unordered ? 0x30000000U : equal ? 0x60000000U
                   : less ? 0x80000000U : 0x20000000U,
        exceptions | floating::exceptions() };
}
}
