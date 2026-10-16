/* SPDX-License-Identifier: MPL-2.0 */
#include "vector_memory_emitter.hpp"
#include "arm_memory/vector_transfer.hpp"

namespace ilemu::execution::arm64 {
using namespace oaknut;
using namespace oaknut::util;
VectorMemoryEmitter::Split& VectorMemoryEmitter::Splits::add(
    const arm::Instruction& inst, bool big_endian, Label& completed)
{
    return paths_.emplace_back(inst, big_endian, labels_, completed);
}
void VectorMemoryEmitter::Splits::finish(
    VectorCodeGenerator& code, AddressCache& addresses, VectorRegisters& vectors)
{
    for (auto& path : paths_) {
        // A cached one-page reference can satisfy the instruction before it
        // emits any guards; such a deferred fragment has no incoming branch.
        if (!path.checked.is_bound())
            continue;
        code.l(path.entry);
        VectorMemoryEmitter { code, path.big_endian, addresses, vectors, labels_ }
            .split_access(path.instruction, path.instruction.load ? X25 : X26,
                path.checked, path.completed);
    }
}
bool VectorMemoryEmitter::supports_split(const arm::Instruction& inst)
{
    const auto& v = std::get<arm::VectorTransferOperands>(inst.vector);
    const bool scalar = v.mode == arm::VectorTransferMode::ScalarMultiple;
    if (!scalar && v.mode != arm::VectorTransferMode::Multiple)
        return false;
    const auto length = arm_memory::vector_transfer_length(v);
    // Bound the heat-earned unrolled fallback. Cold and larger transfers keep
    // the existing checked transaction, including its precise fault prefix.
    constexpr unsigned maximum_split_bytes = 64;
    return length > v.alignment && length <= maximum_split_bytes;
}
void VectorMemoryEmitter::split_access(const arm::Instruction& inst, XReg table,
    Label& checked, Label& completed)
{
    const auto& v = std::get<arm::VectorTransferOperands>(inst.vector);
    const auto length = arm_memory::vector_transfer_length(v);
    // Encodings may promise less alignment than the actual address. Keep
    // unaligned words on the existing checked transaction; earlier guards
    // still enforce the instruction and runtime alignment policy.
    if (v.alignment < 4) {
        code_.AND(W17, W15, 3);
        code_.CBNZ(W17, checked);
    }
    // At most two pages, including modulo32 wrap. Validate both before any
    // architectural update; unavailable/observable views use checked accesses.
    for (unsigned offset : { 0U, length - 1U }) {
        code_.ADD(W17, W15, offset);
        // W21 carries the runtime-selected alignment into checked exits.
        code_.LSR(W17, W17, 12);
        code_.LDR(X17, table, W17, IndexExt::UXTW, 3);
        code_.CBZ(X17, checked);
    }
    const auto first = v.first *
        (v.mode == arm::VectorTransferMode::ScalarMultiple ? v.element_size / 4U : 2U);
    const auto endian_size = v.element_size < 4 ? v.element_size : 4U;
    for (unsigned offset = 0; offset < length; offset += 4) {
        // Checked alignment guarantees each word fits one page. Host pages
        // need not be contiguous, so resolve every word with its own bias.
        code_.ADD(W17, W15, offset);
        code_.LSR(W21, W17, 12);
        code_.LDR(X21, table, W21, IndexExt::UXTW, 3);
        code_.ADD(X21, X21, X17);
        const auto word = first + (big_endian_ && v.element_size == 8
                ? (offset / 4U) ^ 1U : offset / 4U);
        if (inst.load) {
            code_.LDR(S0, X21);
            if (big_endian_)
                endian(Q0, endian_size, false);
            vectors_.write_s(word, Q0);
        } else {
            auto source = vectors_.scalar_s(word, Q0);
            if (big_endian_) {
                code_.FMOV(S0, source);
                endian(Q0, endian_size, false);
                source = S0;
            }
            code_.STR(source, X21);
        }
    }
    addresses_.writeback(inst);
    // The ordinary path may have published a one-page AddressCache entry.
    // This path has no such pointer: retire precisely and reenter through the
    // dispatcher, never a local backedge that could reuse that cached address.
    code_.B(completed);
}
}
