/* SPDX-License-Identifier: MPL-2.0 */
#include "vfp_scalar.hpp"

namespace ilemu::arm {
bool decode_vfp_scalar(Instruction& out, std::uint32_t word, bool thumb,
    execution::VfpRegisterProfile bank,
    execution::ArmInstructionProfile architecture) noexcept
{
    // VLDR/VSTR: coprocessor 10/11, P=1, W=0. The other PUW
    // combinations remain with the multiple/core-register transfer decoders.
    if ((word & 0x0f200e00U) != 0x0d000a00U ||
        (thumb ? (word >> 28U) != 14U : out.condition == 15))
        return false;
    const bool single = (word & (1U << 8U)) == 0;
    const auto high = (word >> 22U) & 1U, vd = (word >> 12U) & 15U;
    const auto first = single ? vd * 2U + high : high * 16U + vd;
    const auto rn = (word >> 16U) & 15U;
    const bool load = (word & (1U << 20U)) != 0;
    if ((thumb && (architecture == execution::ArmInstructionProfile::Armv6 ||
                      (rn == 15 && !load))) ||
        (!single && bank == execution::VfpRegisterProfile::D16 && first >= 16))
        return true;
    out.kind = InstructionKind::VectorTransfer;
    out.rn = rn;
    out.load = load;
    out.add = (word & (1U << 23U)) != 0;
    out.index = true;
    out.align_pc = load && rn == 15;
    out.immediate = (word & 255U) * 4U;
    out.vector = VectorTransferOperands { first, 1U, single ? 4U : 8U, 4U,
        VectorTransferMode::ScalarMultiple, 0U };
    return true;
}
}
