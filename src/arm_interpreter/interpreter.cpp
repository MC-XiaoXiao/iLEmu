/* SPDX-License-Identifier: MPL-2.0 */
#include "arm_interpreter/interpreter.hpp"
#include "arm/a32_decode.hpp"
#include "arm/integer_semantics.hpp"
#include "arm_memory/transfer.hpp"

namespace ilemu::execution {
namespace {
    std::uint32_t read_register(const CpuThreadState& state, unsigned reg)
    {
        return reg == 15 ? state.registers[15] + 8U : state.registers[reg];
    }

    void data_processing(CpuThreadState& state, const arm::A32Instruction& inst)
    {
        const bool carry = (state.cpsr & (1U << 29)) != 0;
        const auto shifted =
            inst.immediate_operand
                ? arm::ShiftResult { inst.immediate,
                      inst.shift_amount == 0 ? carry
                                             : (inst.immediate >> 31) != 0 }
                : arm::shift(read_register(state, inst.rm), inst.shift,
                      inst.register_shift ? state.registers[inst.rs] & 255U
                                          : inst.shift_amount,
                      carry, inst.register_shift);
        const auto a = read_register(state, inst.rn), b = shifted.value;
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
        // This first independent executor supports the common userland A32
        // integer subset. Other states return before mutating guest context.
        if ((state.cpsr & (0x3fU | 0x01000000U | 0x0600fc00U)) != 0x10U) {
            result.reason = StopReason::UnsupportedInstruction;
            return result;
        }
        if ((result.pc & 3U) != 0) {
            result.reason = StopReason::FetchFault;
            return result;
        }
        result.instruction = source.fetch32(result.pc);
        if (!result.instruction) {
            result.reason = StopReason::FetchFault;
            return result;
        }
        const auto inst = arm::decode_a32(*result.instruction);
        const bool passed = arm::condition_passed(inst.condition, state.cpsr);
        if (inst.condition == 15 ||
            (passed && (inst.kind == arm::InstructionKind::Unsupported ||
                           (inst.kind == arm::InstructionKind::Transfer &&
                               !source.data_memory())))) {
            result.reason = StopReason::UnsupportedInstruction;
            return result;
        }
        std::uint32_t branch_target = 0;
        if (passed && inst.kind == arm::InstructionKind::BranchExchange) {
            branch_target = read_register(state, inst.rm);
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
        if (passed) {
            switch (inst.kind) {
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
                    state.registers[14] = result.pc + 4U;
                break;
            case arm::InstructionKind::BranchExchange:
                if (inst.link)
                    state.registers[14] = result.pc + 4U;
                state.cpsr =
                    (state.cpsr & ~(1U << 5)) | ((branch_target & 1U) << 5);
                break;
            default:
                break;
            }
        }
        if (!(passed && inst.kind == arm::InstructionKind::Transfer))
            state.registers[15] =
                passed && inst.kind == arm::InstructionKind::Branch
                    ? result.pc + 8U + inst.immediate
                : passed && inst.kind == arm::InstructionKind::BranchExchange
                    ? branch_target & ((branch_target & 1U) != 0 ? ~1U : ~3U)
                    : result.pc + 4U;
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
