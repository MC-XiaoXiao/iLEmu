/* SPDX-License-Identifier: MPL-2.0 */
#include "floating_emitter.hpp"
namespace ilemu::execution::arm64 {
void FloatingEmitter::comparison(
    const arm::FloatingComparisonOperands& v, bool preserve_apsr)
{
    using namespace oaknut;
    using namespace oaknut::util;
    const auto emit = [&](auto first, auto second) {
        if (v.signal_all_nans)
            code_.FCMPE(first, second);
        else
            code_.FCMP(first, second);
    };
    if (preserve_apsr)
        code_.MRS(X21, SystemReg::NZCV);
    if (v.wide) {
        const auto first = vectors_.scalar_d(v.first / 2U, Q0);
        if (v.with_zero)
            emit(first, ImmConstFZero { 0.0 });
        else {
            const auto second = vectors_.scalar_d(v.second / 2U, Q1);
            emit(first, second);
        }
    } else {
        const auto first = vectors_.scalar_s(v.first, Q0);
        if (v.with_zero)
            emit(first, ImmConstFZero { 0.0 });
        else {
            const auto second = vectors_.scalar_s(v.second, Q1);
            emit(first, second);
        }
    }
    code_.MRS(X29, SystemReg::NZCV);
    if (preserve_apsr)
        code_.MSR(SystemReg::NZCV, X21);
    // Cumulative exceptions remain in FPSR until the environment synchronizes.
}
}
