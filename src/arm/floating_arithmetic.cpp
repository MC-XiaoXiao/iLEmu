/* SPDX-License-Identifier: MPL-2.0 */
#include "arm/floating_arithmetic.hpp"
#include "floating_context.hpp"
namespace ilemu::arm {
namespace {
    class ScalarArithmetic {
    public:
        ScalarArithmetic(bool wide, std::uint32_t fpscr)
            : wide_(wide)
            , fpscr_(fpscr)
        {
        }
        std::uint64_t sign() const
        {
            return wide_ ? 0x8000000000000000ULL : 0x80000000ULL;
        }
        std::uint64_t input(std::uint64_t value)
        {
            if (!wide_)
                value = static_cast<std::uint32_t>(value);
            if ((fpscr_ & (1U << 24U)) != 0 && (value & exponent()) == 0 &&
                (value & fraction()) != 0) {
                value &= sign();
                flags_ |= 0x80U;
            }
            return value;
        }
        std::uint64_t add(std::uint64_t a, std::uint64_t b)
        {
            return evaluate([&] {
                return wide_ ? f64_add({ a }, { b }).v
                             : f32_add({ static_cast<std::uint32_t>(a) },
                                   { static_cast<std::uint32_t>(b) })
                                   .v;
            });
        }
        std::uint64_t subtract(std::uint64_t a, std::uint64_t b)
        {
            return evaluate([&] {
                return wide_ ? f64_sub({ a }, { b }).v
                             : f32_sub({ static_cast<std::uint32_t>(a) },
                                   { static_cast<std::uint32_t>(b) })
                                   .v;
            });
        }
        std::uint64_t multiply(std::uint64_t a, std::uint64_t b)
        {
            return evaluate([&] {
                return wide_ ? f64_mul({ a }, { b }).v
                             : f32_mul({ static_cast<std::uint32_t>(a) },
                                   { static_cast<std::uint32_t>(b) })
                                   .v;
            });
        }
        std::uint64_t divide(std::uint64_t a, std::uint64_t b)
        {
            return evaluate([&] {
                return wide_ ? f64_div({ a }, { b }).v
                             : f32_div({ static_cast<std::uint32_t>(a) },
                                   { static_cast<std::uint32_t>(b) })
                                   .v;
            });
        }
        std::uint64_t square_root(std::uint64_t a)
        {
            return evaluate([&] {
                return wide_ ? f64_sqrt({ a }).v
                             : f32_sqrt({ static_cast<std::uint32_t>(a) }).v;
            });
        }
        std::uint32_t exceptions() const { return flags_; }

    private:
        std::uint64_t exponent() const
        {
            return wide_ ? 0x7ff0000000000000ULL : 0x7f800000ULL;
        }
        std::uint64_t fraction() const
        {
            return wide_ ? 0x000fffffffffffffULL : 0x007fffffULL;
        }
        template <class Function> std::uint64_t evaluate(Function calculate)
        {
            softfloat_exceptionFlags = 0;
            auto value = calculate();
            auto flags = floating::exceptions();
            // Tininess is tested before rounding. An inexact tiny value may
            // have rounded to the smallest normal; an exact tiny result has no
            // UF flag.
            const bool tiny =
                (flags & 8U) != 0 ||
                ((value & exponent()) == 0 && (value & fraction()) != 0);
            if ((fpscr_ & (1U << 24U)) != 0 && tiny) {
                value &= sign();
                flags = (flags & ~16U) | 8U;
            }
            if ((fpscr_ & (1U << 25U)) != 0 &&
                (value & exponent()) == exponent() && (value & fraction()) != 0)
                value = wide_ ? 0x7ff8000000000000ULL : 0x7fc00000U;
            flags_ |= flags;
            return value;
        }
        bool wide_;
        std::uint32_t fpscr_, flags_ = 0;
    };
}
FloatingArithmeticResult calculate_floating(const FloatingArithmeticOperands& v,
    std::uint64_t first, std::uint64_t second, std::uint64_t accumulator,
    std::uint32_t fpscr) noexcept
{
    using Op = FloatingArithmeticOperation;
    ScalarArithmetic arithmetic(v.wide, fpscr);
    if (floating_is_bitwise(v.operation)) {
        if (!v.wide)
            first = static_cast<std::uint32_t>(first);
        if (v.operation == Op::Absolute)
            first &= ~arithmetic.sign();
        if (v.operation == Op::Negate)
            first ^= arithmetic.sign();
        return { first, 0 };
    }
    floating::Context context((fpscr >> 22U) & 3U);
    const auto a = arithmetic.input(first);
    if (v.operation == Op::SquareRoot)
        return { arithmetic.square_root(a), arithmetic.exceptions() };
    const auto b = arithmetic.input(second);
    std::uint64_t result;
    switch (v.operation) {
    case Op::Add:
        result = arithmetic.add(a, b);
        break;
    case Op::Subtract:
        result = arithmetic.subtract(a, b);
        break;
    case Op::Divide:
        result = arithmetic.divide(a, b);
        break;
    default: {
        result = arithmetic.multiply(a, b);
        if (v.operation == Op::NegatedMultiply ||
            v.operation == Op::MultiplySubtract ||
            v.operation == Op::NegatedMultiplyAdd)
            result ^= arithmetic.sign();
        if (v.operation == Op::MultiplyAdd ||
            v.operation == Op::MultiplySubtract ||
            v.operation == Op::NegatedMultiplyAdd ||
            v.operation == Op::NegatedMultiplySubtract) {
            auto d = arithmetic.input(accumulator);
            if (v.operation == Op::NegatedMultiplyAdd ||
                v.operation == Op::NegatedMultiplySubtract)
                d ^= arithmetic.sign();
            // A second operation unpacks the rounded product (including FZ).
            result = arithmetic.add(d, arithmetic.input(result));
        }
        break;
    }
    }
    return { result, arithmetic.exceptions() };
}
}
