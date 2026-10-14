/* SPDX-License-Identifier: MPL-2.0 */
#include "native_continuations.hpp"
#include "compiler.hpp"
#include "native_frame.hpp"
#include <span>
#include <stdexcept>
#include <oaknut/oaknut.hpp>
namespace ilemu::execution::arm64 {
NativeContinuations::NativeContinuations(CodeAllocator& allocator)
{
    using namespace oaknut;
    using namespace oaknut::util;
    std::vector<std::uint32_t> words;
    VectorCodeGenerator code(words);
    Label leave, no_heat, budget_ok, groups_ready, resume;
    // The trace has published all architectural state and restored host FP
    // controls and callee-saved vector registers before entering this thunk.
    code.LDR(X21, X20, offsetof(NativeOutcome, active_continuation));
    code.LDR(X22, X20, offsetof(NativeOutcome, ticks));
    code.LDR(X23, X21, offsetof(NativeContinuation, visits));
    code.ADD(X23, X23, 1);
    code.STR(X23, X21, offsetof(NativeContinuation, visits));
    code.LDR(X23, X21, offsetof(NativeContinuation, maximum_ticks));
    code.UDIV(X24, X22, X23);
    code.CBZ(X24, no_heat);
    code.SUB(X24, X24, 1);
    code.LDR(X23, X21, offsetof(NativeContinuation, heat));
    code.ADD(X23, X23, X24);
    code.STR(X23, X21, offsetof(NativeContinuation, heat));
    code.l(no_heat);
    code.LDR(X23, X20, offsetof(NativeOutcome, continuation_ticks));
    code.ADD(X23, X23, X22);
    code.STR(X23, X20, offsetof(NativeOutcome, continuation_ticks));
    code.STR(X23, X20, offsetof(NativeOutcome, ticks));
    code.LDR(W23, X20, offsetof(NativeOutcome, reason));
    code.CBNZ(W23, leave);
    code.LDR(X23, X20, offsetof(NativeOutcome, continuation_remaining));
    code.CMP(X23, X22);
    code.B(Cond::LS, leave);
    code.SUB(X23, X23, X22);
    code.STR(X23, X20, offsetof(NativeOutcome, continuation_remaining));
    code.LDR(W24, X20, offsetof(NativeOutcome, continuation_hops));
    code.CBZ(W24, leave);
    code.SUB(W24, W24, 1);
    code.STR(W24, X20, offsetof(NativeOutcome, continuation_hops));
    code.LDR(W24, X19, offsetof(CpuThreadState, cpsr));
    code.MOV(W25, 0x0100001fU);
    code.AND(W25, W24, W25);
    code.CMP(W25, 0x10U);
    code.B(Cond::NE, leave);
    code.MOV(W16, NativeContinuation::status_mask);
    code.AND(W24, W24, W16);
    code.LDR(W25, X19, 60);
    code.LSR(W26, W25, 1);
    code.EOR(W26, W26, W25, LSR, 6);
    code.EOR(W26, W26, W24, LSR, 5);
    code.EOR(W26, W26, W24, LSR, 10);
    code.EOR(W26, W26, W24, LSR, 25);
    code.AND(W26, W26, 31);
    code.LDR(X27, X20, offsetof(NativeOutcome, continuations));
    code.ADD(X27, X27, X26, LSL, 6);
    code.LDR(X16, X27, offsetof(NativeContinuation, entry));
    code.CBZ(X16, leave);
    code.LDP(W16, W17, X27, offsetof(NativeContinuation, pc));
    code.CMP(W16, W25);
    code.B(Cond::NE, leave);
    code.CMP(W17, W24);
    code.B(Cond::NE, leave);
    code.LDR(X24, X27, offsetof(NativeContinuation, maximum_ticks));
    code.CMP(X24, X23);
    code.B(Cond::CC, budget_ok);
    code.B(Cond::NE, leave);
    code.LDR(W25, X27, offsetof(NativeContinuation, flags));
    code.TBZ(W25, 1, leave);
    code.l(budget_ok);
    code.MOV(X26, 1);
    code.LDR(W25, X27, offsetof(NativeContinuation, flags));
    code.TBZ(W25, 0, groups_ready);
    code.UDIV(X26, X23, X24);
    code.MOV(X25, 1024);
    code.CMP(X26, X25);
    code.CSEL(X26, X26, X25, Cond::CC);
    code.l(groups_ready);
    code.STR(X26, X20, offsetof(NativeOutcome, groups));
    // State is canonical, host FP controls restored. The callback is the same
    // noexcept RunControl poll used by closed cycles and changes no guest data.
    code.LDR(X16, X20, offsetof(NativeOutcome, poll));
    code.LDR(X0, X20, offsetof(NativeOutcome, control));
    code.BLR(X16);
    code.CBZ(W0, resume);
    code.STR(W0, X20, offsetof(NativeOutcome, reason));
    code.MOV(W16, 1);
    code.STR(W16, X20, offsetof(NativeOutcome, continuation_stopped));
    code.STR(WZR, X20, offsetof(NativeOutcome, has_instruction));
    code.B(leave);
    code.l(resume);
    code.STR(X27, X20, offsetof(NativeOutcome, active_continuation));
    code.LDR(W16, X20, offsetof(NativeOutcome, continuation_calls));
    code.ADD(W16, W16, 1);
    code.STR(W16, X20, offsetof(NativeOutcome, continuation_calls));
    code.MOV(X0, X19);
    code.MOV(X1, X20);
    code.LDR(X16, X27, offsetof(NativeContinuation, entry));
    NativeFrame::tail(words);
    code.l(leave);
    NativeFrame::leave(words);
    dispatcher_ = allocator.allocate(words.size() * sizeof(std::uint32_t));
    if (!dispatcher_)
        throw std::bad_alloc {};
    dispatcher_->publish(std::as_bytes(std::span { words }));
    if (!dispatcher_->entry())
        throw std::runtime_error("ARM64 continuation dispatcher was not published");
}
}
