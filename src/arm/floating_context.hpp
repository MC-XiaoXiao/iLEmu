/* SPDX-License-Identifier: MPL-2.0 */
#pragma once
#include <cstdint>
#define THREAD_LOCAL thread_local
extern "C" {
#include "softfloat.h"
}
#undef THREAD_LOCAL

namespace ilemu::arm::floating {
// SoftFloat context is per host thread and restored even for nested use.
class Context {
public:
    explicit Context(unsigned rounding)
        : rounding_(softfloat_roundingMode)
        , flags_(softfloat_exceptionFlags)
        , tininess_(softfloat_detectTininess)
    {
        constexpr unsigned modes[] = { softfloat_round_near_even,
            softfloat_round_max, softfloat_round_min, softfloat_round_minMag };
        softfloat_roundingMode =
            static_cast<std::uint_fast8_t>(modes[rounding]);
        softfloat_exceptionFlags = 0;
        softfloat_detectTininess = softfloat_tininess_beforeRounding;
    }
    ~Context()
    {
        softfloat_roundingMode = rounding_;
        softfloat_exceptionFlags = flags_;
        softfloat_detectTininess = tininess_;
    }

private:
    std::uint_fast8_t rounding_, flags_, tininess_;
};
inline std::uint32_t exceptions()
{
    const auto flags = softfloat_exceptionFlags;
    return static_cast<std::uint32_t>(
        ((flags & 16U) >> 4U) | ((flags & 8U) >> 2U) | (flags & 4U) |
        ((flags & 2U) << 2U) | ((flags & 1U) << 4U));
}
}
