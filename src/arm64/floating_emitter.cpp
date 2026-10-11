/* SPDX-License-Identifier: MPL-2.0 */
#include "floating_emitter.hpp"
namespace ilemu::execution::arm64 {
template <class Scalar>
void FloatingEmitter::integer_result(
    Scalar input, bool signed_integer, bool round_zero)
{
    using namespace oaknut;
    using namespace oaknut::util;
    const auto emit = [&](unsigned rounding) {
        if (signed_integer) {
            switch (rounding) {
            case 0:
                code_.FCVTNS(W16, input);
                break;
            case 1:
                code_.FCVTPS(W16, input);
                break;
            case 2:
                code_.FCVTMS(W16, input);
                break;
            case 3:
                code_.FCVTZS(W16, input);
                break;
            }
        } else {
            switch (rounding) {
            case 0:
                code_.FCVTNU(W16, input);
                break;
            case 1:
                code_.FCVTPU(W16, input);
                break;
            case 2:
                code_.FCVTMU(W16, input);
                break;
            case 3:
                code_.FCVTZU(W16, input);
                break;
            }
        }
    };
    if (round_zero) {
        emit(3);
        return;
    }
    Label lower, nearest, negative, done;
    code_.LDR(W17, X19, offsetof(CpuThreadState, fpscr));
    code_.TBZ(W17, 23, lower);
    code_.TBZ(W17, 22, negative);
    emit(3);
    code_.B(done);
    code_.l(negative);
    emit(2);
    code_.B(done);
    code_.l(lower);
    code_.TBZ(W17, 22, nearest);
    emit(1);
    code_.B(done);
    code_.l(nearest);
    emit(0);
    code_.l(done);
}
void FloatingEmitter::conversion(const arm::FloatingConversionOperands& v)
{
    using namespace oaknut;
    using namespace oaknut::util;
    using Op = arm::FloatingConversionOperation;
    if (v.operation == Op::IntegerToFloat) {
        code_.UMOV(W16, vectors_.read(v.source / 2U).Selem()[v.source % 4U]);
        if (v.destination_double) {
            if (v.signed_integer)
                code_.SCVTF(D0, W16);
            else
                code_.UCVTF(D0, W16);
        } else {
            if (v.signed_integer)
                code_.SCVTF(S0, W16);
            else
                code_.UCVTF(S0, W16);
        }
    } else {
        if (v.source_double)
            vectors_.read_d(Q0, v.source / 2U);
        else
            code_.DUP(
                Q0.S4(), vectors_.read(v.source / 2U).Selem()[v.source % 4U]);
        if (v.operation == Op::FloatToInteger) {
            if (v.source_double)
                integer_result(D0, v.signed_integer, v.round_zero);
            else
                integer_result(S0, v.signed_integer, v.round_zero);
            code_.INS(
                vectors_.write(v.destination / 2U).Selem()[v.destination % 4U],
                W16);
            return;
        }
        if (v.source_double)
            code_.FCVT(S0, D0);
        else
            code_.FCVT(D0, S0);
    }
    if (v.destination_double)
        vectors_.write_d(v.destination / 2U, Q0);
    else
        vectors_.write_s(v.destination, Q0);
}
}
