/* SPDX-License-Identifier: MPL-2.0 */
#include "arm_interpreter/interpreter.hpp"
#include "simd.hpp"
#include "arm/a32_decode.hpp"
#include "arm/integer_semantics.hpp"
#include "arm/t32_decode.hpp"
#include "arm_memory/multiple.hpp"
#include "arm_memory/transfer.hpp"
#include "arm_memory/vector_transfer.hpp"

namespace ilemu::execution {
namespace {
    std::uint32_t read_register(
        const CpuThreadState& state, unsigned reg, const arm::Instruction& inst)
    {
        if (reg != 15)
            return state.registers[reg];
        const auto pc = state.registers[15] + inst.pc_offset;
        return inst.align_pc ? pc & ~3U : pc;
    }

    void data_processing(CpuThreadState& state, const arm::Instruction& inst)
    {
        const bool carry = (state.cpsr & (1U << 29)) != 0;
        const auto shifted =
            inst.immediate_operand
                ? arm::ShiftResult { inst.immediate,
                      inst.shift_amount == 0 ? carry
                                             : (inst.immediate >> 31) != 0 }
                : arm::shift(read_register(state, inst.rm, inst), inst.shift,
                      inst.register_shift ? state.registers[inst.rs] & 255U
                                          : inst.shift_amount,
                      carry, inst.register_shift);
        const auto a = read_register(state, inst.rn, inst), b = shifted.value;
        std::uint32_t value = 0;
        arm::ArithmeticResult arithmetic { };
        bool arithmetic_flags = false;
        switch (inst.opcode) {
        case 0:
        case 8:
            value = a & b;
            break;
        case 1:
        case 9:
            value = a ^ b;
            break;
        case 2:
        case 10:
            arithmetic = arm::add_with_carry(a, ~b, true);
            arithmetic_flags = true;
            break;
        case 3:
            arithmetic = arm::add_with_carry(b, ~a, true);
            arithmetic_flags = true;
            break;
        case 4:
        case 11:
            arithmetic = arm::add_with_carry(a, b, false);
            arithmetic_flags = true;
            break;
        case 5:
            arithmetic = arm::add_with_carry(a, b, carry);
            arithmetic_flags = true;
            break;
        case 6:
            arithmetic = arm::add_with_carry(a, ~b, carry);
            arithmetic_flags = true;
            break;
        case 7:
            arithmetic = arm::add_with_carry(b, ~a, carry);
            arithmetic_flags = true;
            break;
        case 12:
            value = a | b;
            break;
        case 13:
            value = b;
            break;
        case 14:
            value = a & ~b;
            break;
        case 15:
            value = ~b;
            break;
        case arm::opcode_orn:
            value = a | ~b;
            break;
        }
        if (arithmetic_flags)
            value = arithmetic.value;
        if (inst.opcode < 8 || inst.opcode > 11)
            state.registers[inst.rd] = value;
        if (inst.set_flags) {
            state.cpsr = arm::set_nz(state.cpsr, value);
            state.cpsr = (state.cpsr & ~(1U << 29)) |
                         (std::uint32_t { arithmetic_flags ? arithmetic.carry
                                                           : shifted.carry }
                             << 29);
            if (arithmetic_flags)
                state.cpsr = (state.cpsr & ~(1U << 28)) |
                             (std::uint32_t { arithmetic.overflow } << 28);
        }
    }
}

RunResult ArmInterpreter::run(
    CpuThreadState& state, InstructionSource& source, const RunRequest& request)
{
    RunResult result;
    for (;;) {
        result.pc = state.registers[15];
        result.instruction.reset();
        result.memory_fault.reset();
        result.reason = stops_.consume();
        if (result.ticks_consumed >= request.tick_budget &&
            request.mode != ExecutionMode::SingleStep)
            result.reason = result.reason | StopReason::TickBudget;
        if (std::chrono::steady_clock::now() >= request.deadline)
            result.reason = result.reason | StopReason::HostDeadline;
        if (result.reason != StopReason::None)
            return result;
        const bool thumb = (state.cpsr & 0x20U) != 0;
        const auto it = arm::it_state(state.cpsr);
        if ((state.cpsr & (0x1fU | 0x01000000U)) != 0x10U ||
            (!thumb && it != 0)) {
            result.reason = StopReason::UnsupportedInstruction;
            return result;
        }
        if ((result.pc & (thumb ? 1U : 3U)) != 0) {
            result.reason = StopReason::FetchFault;
            return result;
        }
        auto lease = source.acquire_code_lease(result.pc, thumb);
        if (auto fault = source.prepare_instruction_fetch(
                result.pc, thumb ? 2U : 4U)) {
            result.memory_fault = fault;
            result.reason = StopReason::FetchFault;
            return result;
        }
        arm::Instruction inst;
        if (thumb) {
            const auto first = source.fetch16(result.pc);
            std::optional<std::uint16_t> second;
            if (first && arm::thumb_is_wide(*first)) {
                if (auto fault = source.prepare_instruction_fetch(
                        result.pc + 2U, 2U)) {
                    result.memory_fault = fault;
                    result.reason = StopReason::FetchFault;
                    return result;
                }
                second = source.fetch16(result.pc + 2U);
            }
            if (!first || (arm::thumb_is_wide(*first) && !second)) {
                result.reason = StopReason::FetchFault;
                return result;
            }
            result.instruction =
                *first | (std::uint32_t { second.value_or(0) } << 16U);
            inst = arm::decode_t32(*first, second, it);
        } else {
            result.instruction = source.fetch32(result.pc);
            if (result.instruction)
                inst = arm::decode_a32(*result.instruction);
        }
        if (!result.instruction) {
            result.reason = StopReason::FetchFault;
            return result;
        }
        const bool passed = arm::condition_passed(inst.condition, state.cpsr);
        if (inst.condition == 15 ||
            (passed &&
                (inst.kind == arm::InstructionKind::Unsupported ||
                    ((inst.kind == arm::InstructionKind::Transfer ||
                         inst.kind == arm::InstructionKind::MultipleTransfer ||
                         inst.kind == arm::InstructionKind::VectorTransfer) &&
                        !source.data_memory())))) {
            result.reason = StopReason::UnsupportedInstruction;
            return result;
        }
        std::uint32_t branch_target = 0;
        if (passed && inst.kind == arm::InstructionKind::BranchExchange) {
            branch_target = read_register(state, inst.rm, inst);
            if ((branch_target & 3U) == 2U) {
                result.reason = StopReason::UnsupportedInstruction;
                return result;
            }
        }
        const auto ticks =
            source.ticks_for_instruction(result.pc, *result.instruction);
        if (ticks == 0 || ticks > UINT64_MAX - result.ticks_consumed) {
            result.reason = StopReason::InvalidTiming;
            return result;
        }
        if (passed && inst.kind == arm::InstructionKind::Breakpoint) {
            result.ticks_consumed += ticks;
            result.reason = StopReason::Breakpoint;
            if (request.mode == ExecutionMode::SingleStep)
                result.reason = result.reason | StopReason::SingleStep;
            return result;
        }
        if (passed) {
            switch (inst.kind) {
            case arm::InstructionKind::VectorTransfer: {
                auto& memory = *source.data_memory();
                const auto transfer = arm_memory::prepare_vector(state, inst,
                    memory.direct_memory().permits_unaligned != 0);
                lease.reset();
                const auto completion =
                    arm_memory::complete_vector(state, memory, transfer);
                if (completion.reason != StopReason::None) {
                    result.reason = completion.reason;
                    result.memory_fault = completion.fault;
                    return result;
                }
                break;
            }
            case arm::InstructionKind::MultipleTransfer: {
                auto& memory = *source.data_memory();
                const auto offset = memory.direct_memory().pc_store_offset;
                if (!inst.load && (inst.registers & 0x8000U) != 0 &&
                    offset != 8 && offset != 12) {
                    result.reason = StopReason::UnsupportedInstruction;
                    return result;
                }
                const auto transfer =
                    arm_memory::prepare_multiple(state, inst, offset);
                lease.reset();
                const auto completion =
                    arm_memory::complete_multiple(state, memory, transfer);
                if (completion.reason != StopReason::None) {
                    result.reason = completion.reason;
                    result.memory_fault = completion.fault;
                    return result;
                }
                break;
            }
            case arm::InstructionKind::Transfer: {
                auto& memory = *source.data_memory();
                const auto direct = memory.direct_memory();
                if (inst.rd == 15 && !inst.load &&
                    direct.pc_store_offset != 8 &&
                    direct.pc_store_offset != 12) {
                    result.reason = StopReason::UnsupportedInstruction;
                    return result;
                }
                const auto transfer =
                    arm_memory::prepare(state, inst, direct.pc_store_offset);
                lease.reset();
                const auto completion =
                    arm_memory::complete(state, memory, transfer);
                if (completion.reason != StopReason::None) {
                    result.reason = completion.reason;
                    result.memory_fault = completion.fault;
                    return result;
                }
                break;
            }
            case arm::InstructionKind::DataProcessing:
                data_processing(state, inst);
                if (inst.rd == 15 && (inst.opcode < 8 || inst.opcode > 11))
                    state.registers[15] &= ~1U; // Thumb ALUWritePC
                break;
            case arm::InstructionKind::PackHalfword: {
                const auto shifted = arm::shift(state.registers[inst.rm],
                    inst.shift, inst.shift_amount, false, false).value;
                const auto a = state.registers[inst.rn];
                state.registers[inst.rd] = inst.shift == arm::ShiftKind::Lsl
                    ? (a & 0xffffU) | (shifted & 0xffff0000U)
                    : (a & 0xffff0000U) | (shifted & 0xffffU);
                break;
            }
            case arm::InstructionKind::VectorDuplicate:
                duplicate_vector(state,
                    std::get<arm::VectorDuplicateOperands>(inst.vector));
                break;
            case arm::InstructionKind::VectorBitwise:
                bitwise_vector(state,
                    std::get<arm::VectorBitwiseOperands>(inst.vector));
                break;
            case arm::InstructionKind::WideImmediate:
                state.registers[inst.rd] =
                    inst.opcode != 0 ? (state.registers[inst.rd] & 0xffffU) |
                                           (inst.immediate << 16U)
                                     : inst.immediate;
                break;
            case arm::InstructionKind::Multiply: {
                const auto value =
                    state.registers[inst.rm] * state.registers[inst.rs] +
                    (inst.accumulate ? state.registers[inst.rn] : 0U);
                state.registers[inst.rd] = value;
                if (inst.set_flags)
                    state.cpsr = arm::set_nz(state.cpsr, value);
                break;
            }
            case arm::InstructionKind::Branch:
                if (inst.link)
                    state.registers[14] =
                        (result.pc + inst.size) | (thumb ? 1U : 0U);
                if (inst.exchange)
                    state.cpsr ^= 0x20U;
                break;
            case arm::InstructionKind::BranchExchange:
                if (inst.link)
                    state.registers[14] =
                        (result.pc + inst.size) | (thumb ? 1U : 0U);
                state.cpsr =
                    (state.cpsr & ~(1U << 5)) | ((branch_target & 1U) << 5);
                break;
            default:
                break;
            }
        }
        const bool alu_pc = inst.kind == arm::InstructionKind::DataProcessing &&
                            inst.rd == 15 &&
                            (inst.opcode < 8 || inst.opcode > 11);
        const bool compare_branch =
            inst.kind == arm::InstructionKind::CompareBranch &&
            ((state.registers[inst.rn] != 0) == (inst.opcode != 0));
        if (!(passed &&
                (inst.kind == arm::InstructionKind::Transfer ||
                    inst.kind == arm::InstructionKind::MultipleTransfer ||
                    inst.kind == arm::InstructionKind::VectorTransfer ||
                    alu_pc)))
            state.registers[15] =
                passed && (inst.kind == arm::InstructionKind::Branch ||
                              compare_branch)
                    ? (inst.align_pc ? (result.pc + inst.pc_offset) & ~3U
                                     : result.pc + inst.pc_offset) +
                          inst.immediate
                : passed && inst.kind == arm::InstructionKind::BranchExchange
                    ? branch_target & ((branch_target & 1U) != 0 ? ~1U : ~3U)
                    : result.pc + inst.size;
        if (thumb)
            state.cpsr = arm::with_it_state(
                state.cpsr, passed && inst.kind == arm::InstructionKind::IfThen
                                ? inst.immediate
                                : arm::advance_it(it));
        result.ticks_consumed += ticks;
        if (passed && inst.kind == arm::InstructionKind::Svc) {
            result.svc = inst.immediate;
            result.reason = StopReason::Svc;
            if (request.mode == ExecutionMode::SingleStep)
                result.reason = result.reason | StopReason::SingleStep;
            return result;
        }
        if (request.mode == ExecutionMode::SingleStep) {
            result.pc = state.registers[15];
            result.reason = StopReason::SingleStep;
            return result;
        }
    }
}
}
