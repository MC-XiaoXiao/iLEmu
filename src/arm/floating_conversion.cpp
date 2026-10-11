/* SPDX-License-Identifier: MPL-2.0 */
#include "arm/floating_conversion.hpp"
#include <bit>
#include "floating_context.hpp"

namespace ilemu::arm {
FloatingConversionResult convert_floating(const FloatingConversionOperands& v,
    std::uint64_t input, std::uint32_t fpscr) noexcept
{
    const auto rounding = v.round_zero ? 3U : (fpscr >> 22U) & 3U;
    floating::Context context(rounding);
    std::uint32_t extra = 0;
    using Op = FloatingConversionOperation;
    if (v.operation != Op::IntegerToFloat) {
        if (!v.source_double)
            input = static_cast<std::uint32_t>(input);
        const auto exponent =
            v.source_double ? 0x7ff0000000000000ULL : 0x7f800000ULL;
        const auto fraction =
            v.source_double ? 0x000fffffffffffffULL : 0x007fffffULL;
        if ((fpscr & (1U << 24U)) != 0 && (input & exponent) == 0 &&
            (input & fraction) != 0) {
            input &= ~(exponent | fraction);
            extra |= 0x80U; // IDC: flushed input denormal.
        }
    }
    std::uint64_t result;
    const float32_t single { static_cast<std::uint32_t>(input) };
    const float64_t wide { input };
    if (v.operation == Op::IntegerToFloat) {
        const auto unsigned_value = static_cast<std::uint32_t>(input);
        const auto signed_value = std::bit_cast<std::int32_t>(unsigned_value);
        if (v.destination_double)
            result = v.signed_integer ? i32_to_f64(signed_value).v
                                      : ui32_to_f64(unsigned_value).v;
        else
            result = v.signed_integer ? i32_to_f32(signed_value).v
                                      : ui32_to_f32(unsigned_value).v;
    } else if (v.operation == Op::FloatToInteger) {
        const auto mode = softfloat_roundingMode;
        if (v.signed_integer)
            result = static_cast<std::uint32_t>(
                v.source_double ? f64_to_i32(wide, mode, true)
                                : f32_to_i32(single, mode, true));
        else
            result = static_cast<std::uint32_t>(
                v.source_double ? f64_to_ui32(wide, mode, true)
                                : f32_to_ui32(single, mode, true));
    } else {
        // ARM flushes an unrounded tiny narrowing result before rounding,
        // with UFC alone; an input already flushed above only reports IDC.
        const auto magnitude = input & 0x7fffffffffffffffULL;
        if (v.source_double && (fpscr & (1U << 24U)) != 0 && magnitude != 0 &&
            magnitude < 0x3810000000000000ULL)
            return { (input >> 32U) & 0x80000000U, extra | 8U };
        result = v.source_double ? f64_to_f32(wide).v : f32_to_f64(single).v;
        if ((fpscr & (1U << 25U)) != 0) {
            const auto exp =
                v.destination_double ? 0x7ff0000000000000ULL : 0x7f800000ULL;
            const auto frac =
                v.destination_double ? 0x000fffffffffffffULL : 0x007fffffULL;
            if ((result & exp) == exp && (result & frac) != 0)
                result =
                    v.destination_double ? 0x7ff8000000000000ULL : 0x7fc00000U;
        }
    }
    return { result, extra | floating::exceptions() };
}
}
