/* SPDX-License-Identifier: MPL-2.0 */
#pragma once
#include "arm/instruction.hpp"
#include "execution/arm_state.hpp"
#include <cstddef>
#include <oaknut/oaknut.hpp>
namespace ilemu::execution::arm64 {
// A trace owns the hardware FP environment only between host callbacks.
class FloatingEnvironment {
public:
    explicit FloatingEnvironment(bool standard_controls = false)
        : standard_controls_(standard_controls) { }
    void use() { used_ = true; }
    // X29 holds FPSCR.NZCV; the remaining bits stay in the thread state.
    // Entry and callback restoration cover conditionally executed first uses.
    void use_status() { status_used_ = true; }
    void standard_controls(oaknut::VectorCodeGenerator& code)
    {
        if (standard_controls_ || controls_ == Controls::Standard)
            return;
        code.MOV(oaknut::util::W16, 0x03000000U);
        code.MSR(oaknut::SystemReg::FPCR, oaknut::util::X16);
        controls_ = Controls::Standard;
        switched_controls_ = true;
    }
    void guest_controls(oaknut::VectorCodeGenerator& code)
    {
        if (standard_controls_ || controls_ == Controls::Guest)
            return;
        code.LDR(oaknut::util::W16, oaknut::util::X19,
            offsetof(CpuThreadState, fpscr));
        code.AND(oaknut::util::W16, oaknut::util::W16, 0x03c00000U);
        code.MSR(oaknut::SystemReg::FPCR, oaknut::util::X16);
        controls_ = Controls::Guest;
    }
    void join(const arm::Instruction& inst)
    {
        if (inst.condition == 14 || standard_controls_)
            return;
        using Kind = arm::InstructionKind;
        switch (inst.kind) {
        case Kind::VectorFloatingArithmetic:
        case Kind::FloatingArithmetic:
        case Kind::FloatingComparison:
        case Kind::FloatingConversion:
        case Kind::FloatingSystem:
            // The skipped path may retain the preceding control mode.
            controls_ = Controls::Unknown;
            break;
        default:
            break;
        }
    }
    void next_group(oaknut::VectorCodeGenerator& code)
    {
        if (!switched_controls_)
            return;
        // Earlier matching indirect edges can join here from either mode.
        // Entry and callback resume both establish guest controls as well.
        controls_ = Controls::Unknown;
        guest_controls(code);
    }
    void enter(oaknut::VectorCodeGenerator& code) const
    {
        using namespace oaknut;
        using namespace oaknut::util;
        if (used_) {
            code.MRS(X16, SystemReg::FPCR);
            code.MRS(X17, SystemReg::FPSR);
            code.STP(X16, X17, SP, 160);
        }
        resume(code);
    }
    void resume(oaknut::VectorCodeGenerator& code) const
    {
        using namespace oaknut;
        using namespace oaknut::util;
        if (status_used_)
            code.LDR(W29, X19, offsetof(CpuThreadState, fpscr));
        if (!used_)
            return;
        if (status_used_)
            code.MOV(W16, W29);
        else
            code.LDR(W16, X19, offsetof(CpuThreadState, fpscr));
        if (standard_controls_)
            code.MOV(W17, 0x03000000U);
        else
            code.AND(W17, W16, 0x03c00000U);
        code.MSR(SystemReg::FPCR, X17);
        code.MOV(W17, 0x9fU);
        code.AND(W16, W16, W17);
        code.MSR(SystemReg::FPSR, X16);
    }
    void synchronize(oaknut::VectorCodeGenerator& code) const
    {
        if (!used_ && !status_used_)
            return;
        using namespace oaknut;
        using namespace oaknut::util;
        if (used_) {
            code.MRS(X16, SystemReg::FPSR);
            code.MOV(W17, 0x9fU);
            code.AND(W16, W16, W17);
        }
        code.LDR(W17, X19, offsetof(CpuThreadState, fpscr));
        if (status_used_) {
            code.BFI(W29, W17, 0, 28);
            if (used_)
                code.ORR(W16, W16, W29);
            else
                code.MOV(W16, W29);
        } else
            code.ORR(W16, W16, W17);
        code.STR(W16, X19, offsetof(CpuThreadState, fpscr));
    }
    void leave(oaknut::VectorCodeGenerator& code) const
    {
        using namespace oaknut;
        using namespace oaknut::util;
        synchronize(code);
        if (!used_)
            return;
        code.LDP(X16, X17, SP, 160);
        code.MSR(SystemReg::FPCR, X16);
        code.MSR(SystemReg::FPSR, X17);
    }

private:
    enum class Controls { Guest, Standard, Unknown };
    Controls controls_ = Controls::Guest;
    bool switched_controls_ = false;
    // A proven Advanced SIMD cycle owns these controls across every path,
    // including entry and restoration after the existing host poll callback.
    bool standard_controls_;
    bool used_ = false;
    bool status_used_ = false;
};
}
