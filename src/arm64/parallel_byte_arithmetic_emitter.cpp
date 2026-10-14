/* SPDX-License-Identifier: MPL-2.0 */
#include "integer_emitter.hpp"
namespace ilemu::execution::arm64 {
void IntegerEmitter::parallel_byte_arithmetic(
    const arm::Instruction& inst, bool subtract)
{
    using namespace oaknut;
    using namespace oaknut::util;
    const WReg n {static_cast<int>(inst.rn)}, m {static_cast<int>(inst.rm)};
    constexpr auto low_bits = 0x7f7f7f7fU;
    constexpr auto high_bits = 0x80808080U;
    code_.AND(W15, n, low_bits);
    code_.AND(W16, m, low_bits);
    if (subtract) {
        // Each forced high bit absorbs its byte's borrow.
        code_.ORR(W15, W15, high_bits);
        code_.SUB(W15, W15, W16);
    } else {
        // Seven-bit sums cannot carry into the next byte.
        code_.ADD(W15, W15, W16);
    }
    code_.EOR(W16, n, m);
    code_.AND(W16, W16, high_bits);
    code_.EOR(W15, W15, W16);
    if (subtract)
        code_.EOR(W15, W15, high_bits);
    if (subtract) {
        code_.BIC(W16, n, m);
        code_.ORN(W17, n, m);
    } else {
        code_.AND(W16, n, m);
        code_.ORR(W17, n, m);
    }
    // Majority of source signs and the wrapped result gives the
    // widened sign; inverting the result instead gives carry-out.
    if (inst.sign_extend)
        code_.AND(W17, W17, W15);
    else
        code_.BIC(W17, W17, W15);
    code_.ORR(W16, W16, W17);
    if (inst.sign_extend)
        code_.MVN(W16, W16);
    code_.LSR(W16, W16, 7);
    code_.AND(W16, W16, 0x01010101U);
    code_.MOV(W17, 0x01020408U);
    code_.MUL(W16, W16, W17);
    code_.UBFX(W16, W16, 24, 4);
    code_.BFI(W22, W16, 16, 4);
    code_.MOV(WReg {static_cast<int>(inst.rd)}, W15);
}
}
