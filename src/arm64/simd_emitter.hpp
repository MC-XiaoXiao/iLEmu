/* SPDX-License-Identifier: MPL-2.0 */
#pragma once
#include "arm/instruction.hpp"
#include "vector_registers.hpp"
#include <oaknut/oaknut.hpp>

namespace ilemu::execution::arm64 {
class SimdEmitter {
public:
    SimdEmitter(oaknut::VectorCodeGenerator& code, VectorRegisters& vectors)
        : code_(code)
        , vectors_(vectors)
    {
    }
    void duplicate(const arm::VectorDuplicateOperands&);
    void immediate(const arm::VectorImmediateOperands&);
    void bitwise(const arm::VectorBitwiseOperands&);
    void extract(const arm::VectorExtractOperands&);
    void reverse(const arm::VectorReverseOperands&);
    void shift(const arm::VectorShiftImmediateOperands&);
    void conversion(const arm::VectorFloatingConversionOperands&);
    void core_move(const arm::VectorCoreMoveOperands&);
    void floating(const arm::VectorFloatingArithmeticOperands&);
    void integer(const arm::VectorIntegerOperands&);

private:
    oaknut::QReg source(oaknut::QReg scratch, unsigned d, bool quad);
    oaknut::VectorCodeGenerator& code_;
    VectorRegisters& vectors_;
};
}
