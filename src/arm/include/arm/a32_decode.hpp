/* SPDX-License-Identifier: MPL-2.0 */
#pragma once
#include <cstdint>

namespace ilemu::arm {

enum class InstructionKind {
    Unsupported,
    DataProcessing,
    Multiply,
    Transfer,
    Branch,
    BranchExchange,
    Svc
};
enum class ShiftKind { Lsl, Lsr, Asr, Ror };

// Only validated instruction families enter an executor. Unimplemented or
// unpredictable encodings remain explicit; they are never treated as NOPs.
struct A32Instruction {
    InstructionKind kind = InstructionKind::Unsupported;
    unsigned condition = 14;
    unsigned opcode = 0, rd = 0, rn = 0, rm = 0, rs = 0;
    ShiftKind shift = ShiftKind::Lsl;
    unsigned shift_amount = 0;
    std::uint32_t immediate = 0;
    bool immediate_operand = false;
    bool register_shift = false;
    bool set_flags = false;
    bool link = false;
    bool accumulate = false;
    unsigned access_size = 0;
    bool load = false, sign_extend = false;
    bool add = true, index = true, writeback = false;
};

A32Instruction decode_a32(std::uint32_t word) noexcept;

}
