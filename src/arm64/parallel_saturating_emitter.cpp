/* SPDX-License-Identifier: MPL-2.0 */
#include "arm/parallel_saturating.hpp"
#include "integer_emitter.hpp"
namespace ilemu::execution::arm64 {
unsigned IntegerEmitter::parallel_source(
    unsigned reg, bool exchange, unsigned protected_mask)
{
    using namespace oaknut;
    for (unsigned slot = 0; slot < parallel_values_.size(); ++slot) {
        const auto& value = parallel_values_[slot];
        if (value && value->reg == reg && value->exchange == exchange)
            return slot;
    }
    unsigned slot = 0;
    while ((protected_mask & (1U << slot)) != 0)
        ++slot;
    const SReg scalar { static_cast<int>(slot) };
    code_.FMOV(scalar, WReg { static_cast<int>(reg) });
    if (exchange) {
        const auto lanes = DReg { static_cast<int>(slot) }.H4();
        code_.REV32(lanes, lanes);
    }
    parallel_values_[slot] = ParallelValue { reg, exchange };
    return slot;
}
void IntegerEmitter::parallel_saturating(const arm::Instruction& inst)
{
    using namespace oaknut;
    using Op = arm::ParallelSaturatingOperation;
    if (inst.condition != 14)
        clear_parallel_values();
    const auto operation = static_cast<Op>(inst.opcode);
    const auto layout = arm::parallel_saturating_layout(operation);
    const bool signed_values = operation >= Op::SignedAdd16;
    // V0..V2 are existing trace scratch registers, independent of guest banks.
    // GPR results stay current at every paid instruction; no deferred writes.
    const auto first = parallel_source(inst.rn, false, 0);
    const auto second = parallel_source(inst.rm, layout.exchange, 1U << first);
    const DReg a { static_cast<int>(first) }, b { static_cast<int>(second) };
    switch (operation) {
    case Op::SignedAdd8:
    case Op::UnsignedAdd8:
        if (signed_values)
            code_.SQADD(a.B8(), a.B8(), b.B8());
        else
            code_.UQADD(a.B8(), a.B8(), b.B8());
        break;
    case Op::SignedSubtract8:
    case Op::UnsignedSubtract8:
        if (signed_values)
            code_.SQSUB(a.B8(), a.B8(), b.B8());
        else
            code_.UQSUB(a.B8(), a.B8(), b.B8());
        break;
    case Op::SignedAdd16:
    case Op::UnsignedAdd16:
        if (signed_values)
            code_.SQADD(a.H4(), a.H4(), b.H4());
        else
            code_.UQADD(a.H4(), a.H4(), b.H4());
        break;
    case Op::SignedSubtract16:
    case Op::UnsignedSubtract16:
        if (signed_values)
            code_.SQSUB(a.H4(), a.H4(), b.H4());
        else
            code_.UQSUB(a.H4(), a.H4(), b.H4());
        break;
    case Op::SignedAddSubtract:
    case Op::SignedSubtractAdd:
    case Op::UnsignedAddSubtract:
    case Op::UnsignedSubtractAdd: {
        unsigned scratch = 0;
        while (scratch == first || scratch == second)
            ++scratch;
        parallel_values_[scratch].reset();
        const DReg sum { static_cast<int>(scratch) };
        if (signed_values) {
            code_.SQADD(sum.H4(), a.H4(), b.H4());
            code_.SQSUB(a.H4(), a.H4(), b.H4());
        } else {
            code_.UQADD(sum.H4(), a.H4(), b.H4());
            code_.UQSUB(a.H4(), a.H4(), b.H4());
        }
        const auto lane = layout.subtract_low ? 1U : 0U;
        code_.INS(QReg { a.index() }.Helem()[lane],
            QReg { sum.index() }.Helem()[lane]);
        break;
    }
    }
    code_.FMOV(WReg { static_cast<int>(inst.rd) }, SReg { a.index() });
    for (auto& value : parallel_values_)
        if (value && value->reg == inst.rd)
            value.reset();
    parallel_values_[first] = ParallelValue { inst.rd, false };
    if (inst.condition != 14)
        clear_parallel_values();
}
}
