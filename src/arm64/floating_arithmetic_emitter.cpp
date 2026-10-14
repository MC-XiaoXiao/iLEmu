/* SPDX-License-Identifier: MPL-2.0 */
#include "floating_emitter.hpp"
namespace ilemu::execution::arm64 {
template <class Scalar>
void FloatingEmitter::arithmetic_result(arm::FloatingArithmeticOperation op,
    Scalar destination, Scalar first, Scalar second)
{
    using Op = arm::FloatingArithmeticOperation;
    switch (op) {
    case Op::Add: code_.FADD(destination, first, second); break;
    case Op::Subtract: code_.FSUB(destination, first, second); break;
    case Op::Multiply: code_.FMUL(destination, first, second); break;
    case Op::Divide: code_.FDIV(destination, first, second); break;
    case Op::SquareRoot: code_.FSQRT(destination, first); break;
    case Op::Move: code_.FMOV(destination, first); break;
    case Op::Absolute: code_.FABS(destination, first); break;
    case Op::Negate: code_.FNEG(destination, first); break;
    default:
        // Legacy multiply-accumulate rounds the product before the addition.
        code_.FMUL(destination, first, second);
        if (op == Op::NegatedMultiply || op == Op::MultiplySubtract ||
            op == Op::NegatedMultiplyAdd)
            code_.FNEG(destination, destination);
        break;
    }
}
void FloatingEmitter::arithmetic(const arm::FloatingArithmeticOperands& v)
{
    using namespace oaknut;
    using namespace oaknut::util;
    using Op = arm::FloatingArithmeticOperation;
    const bool accumulate = v.operation == Op::MultiplyAdd ||
        v.operation == Op::MultiplySubtract || v.operation == Op::NegatedMultiplyAdd ||
        v.operation == Op::NegatedMultiplySubtract;
    if (!accumulate)
        product_.reset();
    if (v.operation == Op::Move) {
        if (v.wide) {
            if (auto destination = vectors_.resident_d(v.destination / 2U)) {
                code_.FMOV(*destination, vectors_.scalar_d(v.first / 2U, Q1));
                return;
            }
            if (auto source = vectors_.resident_d(v.first / 2U)) {
                vectors_.write_d(v.destination / 2U, QReg { source->index() });
                return;
            }
            if (vectors_.resident_pair(v.destination / 2U) ||
                vectors_.resident_pair(v.first / 2U)) {
                vectors_.read_d(Q0, v.first / 2U);
                vectors_.write_d(v.destination / 2U, Q0);
                return;
            }
        } else {
            if (auto destination = vectors_.resident_s(v.destination)) {
                code_.FMOV(*destination, vectors_.scalar_s(v.first, Q1));
                return;
            }
            if (auto source = vectors_.resident_s(v.first)) {
                vectors_.write_s(v.destination, QReg { source->index() });
                return;
            }
            if (vectors_.resident_d(v.destination / 2U) ||
                vectors_.resident_d(v.first / 2U)) {
                code_.FMOV(S0, vectors_.scalar_s(v.first, Q1));
                vectors_.write_s(v.destination, Q0);
                return;
            }
        }
        // A lane copy preserves all neighbouring architectural lanes without
        // first materializing the scalar in a temporary vector register.
        if (v.wide)
            code_.INS(vectors_.write(v.destination / 2U).Delem()[(v.destination / 2U) % 2U],
                vectors_.read(v.first / 2U).Delem()[(v.first / 2U) % 2U]);
        else
            code_.INS(vectors_.write(v.destination / 2U).Selem()[v.destination % 4U],
                vectors_.read(v.first / 2U).Selem()[v.first % 4U]);
        return;
    }
    const bool unary = v.operation == Op::SquareRoot ||
        v.operation == Op::Absolute || v.operation == Op::Negate;
    const auto emit = [&](auto destination, const auto& scalar) {
        const decltype(destination) product { accumulate ? 0 : destination.index() };
        const bool negated = v.operation == Op::MultiplySubtract ||
                             v.operation == Op::NegatedMultiplyAdd;
        const bool feedback = v.destination == v.first || v.destination == v.second;
        // Source feedback needs the shorter ordinary multiply/add dependency
        // chain; the unit-multiplier form is for independent accumulators.
        const auto unit = accumulate && !feedback
            ? vectors_.accumulation_unit(v.wide) : std::nullopt;
        bool product_negated = negated;
        // A sign toggle would lengthen a source-feedback chain. Recompute the
        // product in parallel instead of chaining two dependent negations.
        const bool reuse = accumulate && unconditional_ && product_ &&
            product_->first == v.first && product_->second == v.second &&
            product_->wide == v.wide && (!feedback || product_->negated == negated);
        if (!reuse) {
            const auto first = scalar(v.first, Q1);
            const auto second = unary || v.first == v.second
                ? first : scalar(v.second, Q2);
            // Round the product before reading an aliased accumulator.
            arithmetic_result(v.operation, product, first, second);
        } else {
            product_negated = product_->negated;
            if (!unit && product_negated != negated) {
                code_.FNEG(product, product);
                product_negated = negated;
            }
        }
        if (accumulate) {
            // The product is now in scratch V0. Inputs are no longer needed;
            // only now may V1 be reused for an upper-lane accumulator.
            auto accumulator = scalar(v.destination, Q1);
            const bool negative_accumulator = v.operation == Op::NegatedMultiplyAdd ||
                                              v.operation == Op::NegatedMultiplySubtract;
            if (unit && (negative_accumulator || product_negated != negated)) {
                const decltype(destination) one { unit->index() };
                // FMUL has already rounded the legacy product. Multiplying
                // that result by 1 preserves the second rounding and lets the
                // instruction negate operands before addend-first NaN handling.
                if (negative_accumulator) {
                    if (product_negated != negated)
                        code_.FNMADD(destination, product, one, accumulator);
                    else
                        code_.FNMSUB(destination, product, one, accumulator);
                } else
                    code_.FMSUB(destination, product, one, accumulator);
            } else {
                if (negative_accumulator) {
                    const decltype(destination) temporary { 1 };
                    code_.FNEG(temporary, accumulator);
                    accumulator = temporary;
                }
                code_.FADD(destination, accumulator, product);
            }
            // Accumulation leaves V0 intact with a resident destination. A write
            // to either source revokes the proof for the next instruction.
            // Repeating this nontrapping multiplication would only reassert
            // already-sticky FPSR flags; every status/control boundary clears it.
            if (unconditional_ && destination.index() != 0 &&
                !feedback)
                product_ = Product { v.first, v.second, v.wide, product_negated };
            else
                product_.reset();
        }
    };
    if (v.wide) {
        const auto resident = vectors_.resident_d(v.destination / 2U);
        emit(resident.value_or(D0), [&](unsigned word, QReg scratch) {
            return vectors_.scalar_d(word / 2U, scratch);
        });
        if (!resident)
            vectors_.write_d(v.destination / 2U, Q0);
    } else {
        const auto resident = vectors_.resident_s(v.destination);
        emit(resident.value_or(S0), [&](unsigned word, QReg scratch) {
            return vectors_.scalar_s(word, scratch);
        });
        if (!resident)
            vectors_.write_s(v.destination, Q0);
    }
}
}
