/* SPDX-License-Identifier: MPL-2.0 */
#pragma once
#include "arm/instruction.hpp"
#include "vector_registers.hpp"
#include <optional>
namespace ilemu::execution::arm64 {
class FloatingEmitter {
public:
    FloatingEmitter(oaknut::VectorCodeGenerator& code, VectorRegisters& vectors)
        : code_(code)
        , vectors_(vectors)
    {
    }
    // Scratch V0 is reusable only across adjacent unconditional arithmetic.
    // Other instructions and conditional paths cannot publish a product proof.
    void observe(const arm::Instruction& inst)
    {
        unconditional_ = inst.kind == arm::InstructionKind::FloatingArithmetic &&
                         inst.condition == 14;
        if (!unconditional_)
            product_.reset();
    }
    void immediate(const arm::FloatingImmediateOperands&, bool unconditional);
    void comparison(const arm::FloatingComparisonOperands&, bool preserve_apsr);
    void arithmetic(const arm::FloatingArithmeticOperands&);
    void conversion(const arm::FloatingConversionOperands&);

private:
    template <class Scalar>
    void arithmetic_result(
        arm::FloatingArithmeticOperation, Scalar destination, Scalar first, Scalar second);
    template <class Scalar>
    void integer_result(Scalar, bool signed_integer, bool round_zero);
    struct Product {
        unsigned first, second;
        bool wide, negated;
    };
    std::optional<Product> product_;
    bool unconditional_ = false;
    oaknut::VectorCodeGenerator& code_;
    VectorRegisters& vectors_;
};
}
