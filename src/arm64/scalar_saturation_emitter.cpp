/* SPDX-License-Identifier: MPL-2.0 */
#include "integer_emitter.hpp"
namespace ilemu::execution::arm64 {
void IntegerEmitter::saturate_scalar(
    unsigned bits, bool signed_result, unsigned source_bits)
{
    using namespace oaknut;
    using namespace oaknut::util;
    if (signed_result && bits >= source_bits)
        return;
    if (!signed_result && bits >= source_bits - 1U) {
        code_.ASR(X17, X16, 63);
        code_.BIC(X16, X16, X17);
        code_.AND(W15, W17, 0x08000000U);
        code_.ORR(W22, W22, W15);
        return;
    }
    const auto maximum =
        (std::uint64_t {1} << (bits - unsigned(signed_result))) - 1U;
    code_.MOV(X17, maximum);
    if (signed_result) {
        // Choose the limit from the original sign. A fitting value equals
        // the sign extension of its low result bits.
        code_.ASR(X15, X16, 63);
        code_.EOR(X17, X17, X15);
        if (bits == 32)
            code_.CMP(X16, W16, AddSubExt::SXTW);
        else {
            code_.SBFX(X15, X16, 0, bits);
            code_.CMP(X16, X15);
        }
    } else {
        // Negative signed inputs also lie above maximum in unsigned order.
        code_.CMP(X16, X17);
        code_.ASR(X15, X16, 63);
        code_.BIC(X17, X17, X15);
    }
    const auto clamped = signed_result ? Cond::NE : Cond::HI;
    code_.CSEL(X16, X17, X16, clamped);
    code_.CSET(W15, clamped);
    code_.ORR(W22, W22, W15, LogShift::LSL, 27);
}
}
