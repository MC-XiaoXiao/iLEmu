/* SPDX-License-Identifier: MPL-2.0 */
#pragma once
#include <cstdint>
#include <variant>

namespace ilemu::arm {

enum class InstructionKind {
    Unsupported,
    DataProcessing,
    Multiply,
    Transfer,
    MultipleTransfer,
    Branch,
    BranchExchange,
    Svc,
    Breakpoint,
    IfThen,
    Nop,
    CompareBranch,
    WideImmediate,
    PackHalfword,
    VectorDuplicate,
    VectorBitwise,
    VectorTransfer
};
enum class ShiftKind { Lsl, Lsr, Asr, Ror };
// Data-processing uses A32 opcode numbers; Thumb adds ORN to that set.
inline constexpr unsigned opcode_orn = 16;

struct VectorDuplicateOperands {
    // Vector indices name D registers; a quad destination names its even D.
    unsigned destination = 0, source = 0;
    unsigned element_bits = 0, lane = 0;
    bool quad = false, core_source = false;
};
enum class VectorBitwiseOperation {
    And, BitClear, Or, OrNot, Xor, Select, InsertIfTrue, InsertIfFalse, Not
};
struct VectorBitwiseOperands {
    unsigned destination = 0, first = 0, second = 0;
    VectorBitwiseOperation operation = VectorBitwiseOperation::And;
    bool quad = false;
};
enum class VectorTransferMode { Multiple, Lane, Replicate };
struct VectorTransferOperands {
    unsigned first = 0, count = 0, element_size = 0, alignment = 1;
    VectorTransferMode mode = VectorTransferMode::Multiple;
    unsigned lane = 0;
};
using VectorOperands = std::variant<VectorDuplicateOperands,
    VectorBitwiseOperands, VectorTransferOperands>;

// Only validated instruction families enter an executor. Unimplemented or
// unpredictable encodings remain explicit; they are never treated as NOPs.
struct Instruction {
    InstructionKind kind = InstructionKind::Unsupported;
    unsigned condition = 14;
    unsigned size = 4, pc_offset = 8;
    bool align_pc = false, exchange = false;
    unsigned opcode = 0, rd = 0, rn = 0, rm = 0, rs = 0;
    ShiftKind shift = ShiftKind::Lsl;
    unsigned shift_amount = 0;
    std::uint32_t immediate = 0;
    bool immediate_operand = false;
    bool register_shift = false;
    bool set_flags = false;
    bool link = false;
    bool accumulate = false;
    std::uint32_t registers = 0;
    unsigned access_size = 0;
    bool load = false, sign_extend = false;
    bool add = true, index = true, writeback = false;
    VectorOperands vector;
};

}
