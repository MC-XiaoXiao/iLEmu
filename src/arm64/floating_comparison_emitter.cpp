/* SPDX-License-Identifier: MPL-2.0 */
#include "floating_emitter.hpp"
namespace ilemu::execution::arm64 {
void FloatingEmitter::comparison(const arm::FloatingComparisonOperands& v)
{
    using namespace oaknut;
    using namespace oaknut::util;
    const auto emit = [&](auto first, auto second) {
        if (v.signal_all_nans)
            code_.FCMPE(first, second);
        else
            code_.FCMP(first, second);
    };
    code_.MRS(X21, SystemReg::NZCV);
    if (v.wide) {
        vectors_.read_d(Q0, v.first / 2U);
        if (v.with_zero)
            emit(D0, ImmConstFZero { 0.0 });
        else {
            vectors_.read_d(Q1, v.second / 2U);
            emit(D0, D1);
        }
    } else {
        code_.DUP(Q0.S4(), vectors_.read(v.first / 2U).Selem()[v.first % 4U]);
        if (v.with_zero)
            emit(S0, ImmConstFZero { 0.0 });
        else {
            code_.DUP(Q1.S4(), vectors_.read(v.second / 2U).Selem()[v.second % 4U]);
            emit(S0, S1);
        }
    }
    code_.MRS(X17, SystemReg::NZCV);
    code_.MSR(SystemReg::NZCV, X21); // VCMP updates FPSCR, preserving APSR.
    code_.LDR(W16, X19, offsetof(CpuThreadState, fpscr));
    code_.BFI(W17, W16, 0, 28);
    code_.STR(W17, X19, offsetof(CpuThreadState, fpscr));
    // Cumulative exceptions remain in FPSR until the environment synchronizes.
}
}
