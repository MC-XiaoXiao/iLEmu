/* SPDX-License-Identifier: MPL-2.0 */
#include "arm_interpreter/interpreter.hpp"
#include "simd.hpp"
#include "arm/a32_decode.hpp"
#include "arm/integer_semantics.hpp"
#include "arm/t32_decode.hpp"
#include "arm/multiply.hpp"
#include "arm/integer_transform.hpp"
#include "arm/floating_conversion.hpp"
#include "arm/floating_arithmetic.hpp"
#include "arm/floating_comparison.hpp"
#include "arm/floating_immediate.hpp"
#include "arm/floating_system.hpp"
#include "arm/pc_write.hpp"
#include "arm/architectural_stop.hpp"
#include "arm_memory/multiple.hpp"
#include "arm_memory/transfer.hpp"
#include "arm_memory/exclusive.hpp"
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

    using arm::AluResult;
    AluResult data_processing(
        const CpuThreadState& state, const arm::Instruction& inst)
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
        return arm::calculate_alu(inst.opcode, inst.set_flags,
            read_register(state, inst.rn, inst), shifted, state.cpsr);
    }
}

RunResult ArmInterpreter::run(
    CpuThreadState& state, InstructionSource& source, const RunRequest& request)
{
    return run_slice(state, source, request, context_, UINT64_MAX);
}

RunResult ArmInterpreter::run_slice(CpuThreadState& state,
    InstructionSource& source, const RunRequest& request, ExecutorContext& context,
    std::uint64_t instruction_limit, std::uint64_t ticks_consumed)
{
    context.bind_memory(source.data_memory());
    RunResult result;
    result.ticks_consumed = ticks_consumed;
    std::uint64_t instructions = 0;
    const auto profile = source.thumb_arithmetic_profile();
    const auto vfp = source.vfp_register_profile();
    const auto floating_instructions = source.vfp_instruction_profile();
    const auto architecture = source.arm_instruction_profile();
    for (;;) {
        result.pc = state.registers[15];
        result.instruction.reset();
        result.memory_fault.reset();
        result.reason = context.stops.consume();
        if (result.ticks_consumed >= request.tick_budget &&
            request.mode != ExecutionMode::SingleStep)
            result.reason = result.reason | StopReason::TickBudget;
        if (request.deadline != std::chrono::steady_clock::time_point::max() &&
            std::chrono::steady_clock::now() >= request.deadline)
            result.reason = result.reason | StopReason::HostDeadline;
        if (result.reason != StopReason::None)
            return result;
        if (instructions == instruction_limit)
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
            inst = arm::decode_t32(*first, second, it, profile, vfp, architecture, floating_instructions);
        } else {
            result.instruction = source.fetch32(result.pc);
            if (result.instruction)
                inst = arm::decode_a32(*result.instruction, vfp, architecture, floating_instructions);
        }
        if (!result.instruction) {
            result.reason = StopReason::FetchFault;
            return result;
        }
        const bool passed = arm::condition_passed(inst.condition, state.cpsr);
        if (inst.condition == 15 ||
            (passed &&
                (inst.kind == arm::InstructionKind::Unsupported ||
                    (inst.kind == arm::InstructionKind::FloatingArithmetic &&
                        (state.fpscr & arm::floating_arithmetic_controls(
                            std::get<arm::FloatingArithmeticOperands>(inst.vector).operation)) != 0) ||
                    (inst.kind == arm::InstructionKind::FloatingImmediate &&
                        (state.fpscr & arm::floating_vector_controls) != 0) ||
                    (inst.kind == arm::InstructionKind::FloatingComparison &&
                        (state.fpscr & arm::floating_comparison_controls) != 0) ||
                    (inst.kind == arm::InstructionKind::FloatingConversion &&
                        (state.fpscr & arm::floating_exception_enables) != 0) ||
                    ((inst.kind == arm::InstructionKind::TableBranch ||
                         inst.kind == arm::InstructionKind::Transfer ||
                         inst.kind == arm::InstructionKind::DualTransfer ||
                         inst.kind == arm::InstructionKind::ExclusiveTransfer ||
                         inst.kind == arm::InstructionKind::Swap ||
                         inst.kind == arm::InstructionKind::MultipleTransfer ||
                         inst.kind == arm::InstructionKind::VectorTransfer) &&
                        !source.data_memory())))) {
            result.reason = StopReason::UnsupportedInstruction;
            return result;
        }
        const bool alu_pc = arm::alu_writes_pc(inst);
        AluResult alu { };
        std::optional<arm::PcWrite> alu_target;
        if (passed && inst.kind == arm::InstructionKind::DataProcessing) {
            alu = data_processing(state, inst);
            if (alu_pc) {
                alu_target = arm::alu_write_pc(
                    alu.value, thumb, source.alu_pc_write_profile());
                if (!alu_target) {
                    result.reason = StopReason::UnsupportedInstruction;
                    return result;
                }
            }
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
        if (passed) {
            if (const auto stop = arm::ArchitecturalStop::from(inst)) {
                stop->retire(state, result, thumb, it, ticks, request.mode);
                return result;
            }
        }
        if (passed) {
            switch (inst.kind) {
            case arm::InstructionKind::FloatingSystem: {
                const auto& v = std::get<arm::FloatingSystemOperands>(inst.vector);
                if (!v.to_core)
                    state.fpscr = state.registers[v.core] &
                        arm::fpscr_write_mask(source.floating_exception_profile());
                else if (v.core == 15)
                    state.cpsr = (state.cpsr & 0x0fffffffU) |
                        (state.fpscr & 0xf0000000U);
                else
                    state.registers[v.core] = state.fpscr;
                break;
            }
            case arm::InstructionKind::ClearExclusive:
            case arm::InstructionKind::Swap:
            case arm::InstructionKind::ExclusiveTransfer: {
                const auto transfer = arm_memory::prepare_exclusive(state, inst);
                lease.reset();
                const auto completion = arm_memory::complete_exclusive(
                    state, source.data_memory(), context.exclusive, transfer);
                if (completion.reason != StopReason::None) {
                    result.reason = completion.reason;
                    result.memory_fault = completion.fault;
                    return result;
                }
                break;
            }
            case arm::InstructionKind::SystemControl:
                switch (inst.system) {
                case arm::SystemOperation::ThreadPointerReadWrite:
                    if (inst.load)
                        state.registers[inst.rd] = state.thread_pointer_user_rw;
                    else
                        state.thread_pointer_user_rw = state.registers[inst.rd];
                    break;
                case arm::SystemOperation::ThreadPointerReadOnly:
                    state.registers[inst.rd] = state.cthread_self.value_or(0);
                    break;
                default:
                    // Checked accesses complete synchronously; the next
                    // instruction reacquires its code view, including for ISB.
                    std::atomic_thread_fence(std::memory_order_seq_cst);
                    break;
                }
                break;
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
            case arm::InstructionKind::TableBranch:
            case arm::InstructionKind::DualTransfer:
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
                if (alu_pc) {
                    state.registers[15] = alu_target->address;
                    state.cpsr = (alu.cpsr & ~0x20U) |
                        (alu_target->thumb ? 0x20U : 0U);
                } else {
                    if (inst.opcode < 8 || inst.opcode > 11)
                        state.registers[inst.rd] = alu.value;
                    state.cpsr = alu.cpsr;
                }
                break;
            case arm::InstructionKind::IntegerTransform:
                state.registers[inst.rd] = arm::integer_transform(inst,
                    state.registers[inst.rm], state.registers[inst.rn]);
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
            case arm::InstructionKind::FloatingArithmetic: {
                const auto& v = std::get<arm::FloatingArithmeticOperands>(inst.vector);
                const auto value = [&](unsigned word) {
                    std::uint64_t result = state.extension_registers[word];
                    if (v.wide)
                        result |= std::uint64_t { state.extension_registers[word + 1U] } << 32U;
                    return result;
                };
                const auto result = arm::calculate_floating(v, value(v.first),
                    value(v.second), value(v.destination), state.fpscr);
                state.extension_registers[v.destination] = static_cast<std::uint32_t>(result.value);
                if (v.wide)
                    state.extension_registers[v.destination + 1U] = static_cast<std::uint32_t>(result.value >> 32U);
                state.fpscr |= result.exceptions;
                break;
            }
            case arm::InstructionKind::FloatingImmediate: {
                const auto& v = std::get<arm::FloatingImmediateOperands>(inst.vector);
                const auto value = arm::expand_floating_immediate(v);
                state.extension_registers[v.destination] = static_cast<std::uint32_t>(value);
                if (v.wide)
                    state.extension_registers[v.destination + 1U] = static_cast<std::uint32_t>(value >> 32U);
                break;
            }
            case arm::InstructionKind::FloatingComparison: {
                const auto& v = std::get<arm::FloatingComparisonOperands>(inst.vector);
                const auto value = [&](unsigned word) {
                    return std::uint64_t { state.extension_registers[word] } |
                        (v.wide ? std::uint64_t { state.extension_registers[word + 1U] } << 32U : 0U);
                };
                const auto comparison = arm::compare_floating(v,
                    value(v.first), v.with_zero ? 0U : value(v.second), state.fpscr);
                state.fpscr = (state.fpscr & 0x0fffffffU) |
                    comparison.nzcv | comparison.exceptions;
                break;
            }
            case arm::InstructionKind::FloatingConversion: {
                const auto& v = std::get<arm::FloatingConversionOperands>(inst.vector);
                std::uint64_t input = state.extension_registers[v.source];
                if (v.source_double)
                    input |= std::uint64_t { state.extension_registers[v.source + 1U] } << 32U;
                const auto converted = arm::convert_floating(v, input, state.fpscr);
                state.extension_registers[v.destination] = static_cast<std::uint32_t>(converted.value);
                if (v.destination_double)
                    state.extension_registers[v.destination + 1U] = static_cast<std::uint32_t>(converted.value >> 32U);
                state.fpscr |= converted.exceptions;
                break;
            }
            case arm::InstructionKind::VectorCoreMove:
                move_core_vector(state,
                    std::get<arm::VectorCoreMoveOperands>(inst.vector));
                break;
            case arm::InstructionKind::VectorDuplicate:
                duplicate_vector(state,
                    std::get<arm::VectorDuplicateOperands>(inst.vector));
                break;
            case arm::InstructionKind::VectorBitwise:
                bitwise_vector(state,
                    std::get<arm::VectorBitwiseOperands>(inst.vector));
                break;
            case arm::InstructionKind::VectorImmediate:
                immediate_vector(state,
                    std::get<arm::VectorImmediateOperands>(inst.vector));
                break;
            case arm::InstructionKind::VectorExtract:
                extract_vector(state,
                    std::get<arm::VectorExtractOperands>(inst.vector));
                break;
            case arm::InstructionKind::WideImmediate:
                state.registers[inst.rd] =
                    inst.opcode != 0 ? (state.registers[inst.rd] & 0xffffU) |
                                           (inst.immediate << 16U)
                                     : inst.immediate;
                break;
            case arm::InstructionKind::Multiply: {
                const auto value = arm::multiply(inst, state.registers[inst.rm],
                    state.registers[inst.rs], state.registers[inst.rd],
                    state.registers[inst.rn]);
                state.registers[inst.rd] = static_cast<std::uint32_t>(value);
                if (arm::multiply_writes_pair(inst))
                    state.registers[inst.rn] = static_cast<std::uint32_t>(value >> 32U);
                if (inst.set_flags)
                    state.cpsr = arm::multiply_nz(inst, state.cpsr, value);
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
        const bool compare_branch =
            inst.kind == arm::InstructionKind::CompareBranch &&
            ((state.registers[inst.rn] != 0) == (inst.opcode != 0));
        if (!(passed &&
                (inst.kind == arm::InstructionKind::TableBranch ||
                    inst.kind == arm::InstructionKind::Transfer ||
                    inst.kind == arm::InstructionKind::DualTransfer ||
                    inst.kind == arm::InstructionKind::ExclusiveTransfer ||
                    inst.kind == arm::InstructionKind::Swap ||
                    inst.kind == arm::InstructionKind::ClearExclusive ||
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
        if (request.mode == ExecutionMode::SingleStep) {
            result.pc = state.registers[15];
            result.reason = StopReason::SingleStep;
            return result;
        }
        ++instructions;
    }
}
}
