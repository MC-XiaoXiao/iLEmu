/* SPDX-License-Identifier: MPL-2.0 */
#include "integer_emitter.hpp"
namespace ilemu::execution::arm64 {
using namespace oaknut;
using namespace oaknut::util;
bool IntegerEmitter::logical_immediate(const arm::Instruction& inst,
    std::uint32_t pc, LogicalFlagOutput flags)
{
    if (!inst.immediate_operand ||
        (inst.set_flags && flags != LogicalFlagOutput::All))
        return false;
    const bool move = inst.opcode == 13 || inst.opcode == 15;
    const bool inverted = inst.opcode == 14 || inst.opcode == 15 ||
                          inst.opcode == arm::opcode_orn;
    const auto immediate = inverted ? ~inst.immediate : inst.immediate;
    // Reuse the assembler's encoding proof; zero/all-ones and other constants
    // retain the general shifter path unless MOV can materialize them directly.
    if (!move && !oaknut::detail::encode_bit_imm(immediate))
        return false;
    const bool test = inst.opcode == 8 || inst.opcode == 9;
    const auto d = test ? WZR : WReg { static_cast<int>(inst.rd) };
    const auto value = test ? W15 : d;
    const auto a = lhs(inst, pc);
    if (inst.set_flags) {
        code_.MRS(X21, SystemReg::NZCV);
        if (inst.shift_amount != 0) {
            // ARM modified immediates publish bit31 of the original operand,
            // including BIC/MVN. V remains from the incoming flags.
            if ((inst.immediate & 0x80000000U) != 0)
                code_.ORR(W21, W21, 0x20000000U);
            else
                code_.AND(W21, W21, 0xdfffffffU);
        }
    }
    const bool combined_nz = inst.set_flags &&
        (inst.opcode == 0 || inst.opcode == 8 || inst.opcode == 14);
    switch (inst.opcode) {
    case 0:
    case 8:
    case 14:
        if (combined_nz)
            code_.ANDS(d, a, BitImm32 { immediate });
        else
            code_.AND(value, a, BitImm32 { immediate });
        break;
    case 1:
    case 9:
        code_.EOR(value, a, BitImm32 { immediate });
        break;
    case 12:
    case arm::opcode_orn:
        code_.ORR(d, a, BitImm32 { immediate });
        break;
    case 13:
    case 15:
        code_.MOV(d, immediate);
        break;
    default:
        return false;
    }
    if (inst.set_flags) {
        if (!combined_nz)
            code_.ANDS(WZR, value, value);
        merge_nz();
    }
    return true;
}
}
