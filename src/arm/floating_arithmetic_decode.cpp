/* SPDX-License-Identifier: MPL-2.0 */
#include "floating_arithmetic_decode.hpp"
namespace ilemu::arm {
bool decode_floating_arithmetic(Instruction& out, std::uint32_t word,
    bool thumb, execution::VfpRegisterProfile bank,
    execution::ArmInstructionProfile architecture) noexcept
{
    if (thumb ? (word >> 28U) != 14U : out.condition == 15)
        return false;
    FloatingArithmeticOperands v;
    using Op = FloatingArithmeticOperation;
    bool unary = false;
    switch (word & 0x0fb00e50U) {
    case 0x0e300a00U:
        v.operation = Op::Add;
        break;
    case 0x0e300a40U:
        v.operation = Op::Subtract;
        break;
    case 0x0e200a00U:
        v.operation = Op::Multiply;
        break;
    case 0x0e800a00U:
        v.operation = Op::Divide;
        break;
    case 0x0e200a40U:
        v.operation = Op::NegatedMultiply;
        break;
    case 0x0e000a00U:
        v.operation = Op::MultiplyAdd;
        break;
    case 0x0e000a40U:
        v.operation = Op::MultiplySubtract;
        break;
    case 0x0e100a40U:
        v.operation = Op::NegatedMultiplyAdd;
        break;
    case 0x0e100a00U:
        v.operation = Op::NegatedMultiplySubtract;
        break;
    default:
        unary = true;
        switch (word & 0x0fbf0ed0U) {
        case 0x0eb10ac0U:
            v.operation = Op::SquareRoot;
            break;
        case 0x0eb00a40U:
            v.operation = Op::Move;
            break;
        case 0x0eb00ac0U:
            v.operation = Op::Absolute;
            break;
        case 0x0eb10a40U:
            v.operation = Op::Negate;
            break;
        default:
            return false;
        }
    }
    v.wide = (word & 256U) != 0;
    const auto reg = [&](unsigned low, unsigned high) {
        return v.wide ? (low + high * 16U) * 2U : low * 2U + high;
    };
    v.destination = reg((word >> 12U) & 15U, (word >> 22U) & 1U);
    v.second = reg(word & 15U, (word >> 5U) & 1U);
    v.first = unary ? v.second : reg((word >> 16U) & 15U, (word >> 7U) & 1U);
    if ((thumb && architecture == execution::ArmInstructionProfile::Armv6) ||
        (bank == execution::VfpRegisterProfile::D16 && v.wide &&
            (v.destination >= 32 || v.first >= 32 || v.second >= 32)))
        return true;
    out.kind = InstructionKind::FloatingArithmetic;
    out.vector = v;
    return true;
}
}
