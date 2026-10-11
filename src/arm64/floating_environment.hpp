/* SPDX-License-Identifier: MPL-2.0 */
#pragma once
#include "execution/arm_state.hpp"
#include <cstddef>
#include <oaknut/oaknut.hpp>
namespace ilemu::execution::arm64 {
// A trace owns the hardware FP environment only between host callbacks.
class FloatingEnvironment {
public:
    void use() { used_ = true; }
    // X29 holds FPSCR.NZCV; the remaining bits stay in the thread state.
    // Entry and callback restoration cover conditionally executed first uses.
    void use_status() { status_used_ = true; }
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
    bool used_ = false;
    bool status_used_ = false;
};
}
