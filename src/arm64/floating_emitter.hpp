/* SPDX-License-Identifier: MPL-2.0 */
#pragma once
#include "arm/instruction.hpp"
#include "vector_registers.hpp"
namespace ilemu::execution::arm64 {
class FloatingEmitter {
public:
    FloatingEmitter(oaknut::VectorCodeGenerator& code, VectorRegisters& vectors)
        : code_(code)
        , vectors_(vectors)
    {
    }
    void immediate(const arm::FloatingImmediateOperands&, bool unconditional);
    void comparison(const arm::FloatingComparisonOperands&);
    void arithmetic(const arm::FloatingArithmeticOperands&);
    void conversion(const arm::FloatingConversionOperands&);

private:
    template <class Scalar>
    void arithmetic_result(
        arm::FloatingArithmeticOperation, Scalar, Scalar, Scalar);
    template <class Scalar>
    void integer_result(Scalar, bool signed_integer, bool round_zero);
    oaknut::VectorCodeGenerator& code_;
    VectorRegisters& vectors_;
};
}
