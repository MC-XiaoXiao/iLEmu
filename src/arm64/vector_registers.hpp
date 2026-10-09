/* SPDX-License-Identifier: MPL-2.0 */
#pragma once
#include "execution/arm_state.hpp"
#include <cstddef>
#include <oaknut/oaknut.hpp>

namespace ilemu::execution::arm64 {
// Sixteen guest Q registers alias all thirty-two D registers. V16..V31 are
// caller-saved; V0..V2 remain scratch and host callee-saved V8..V15 are
// untouched. Masks describe the whole emitted trace, including conditional
// paths. Entry loads preserve neighbours even for a write-only D or lane
// destination.
class VectorRegisters {
public:
    explicit VectorRegisters(oaknut::VectorCodeGenerator& code)
        : code_(code)
    {
    }
    oaknut::QReg read(unsigned d)
    {
        used_ |= 1U << (d / 2U);
        return oaknut::QReg { 16 + static_cast<int>(d / 2U) };
    }
    oaknut::QReg write(unsigned d)
    {
        dirty_ |= 1U << (d / 2U);
        return read(d);
    }
    void read_d(oaknut::QReg scratch, unsigned d)
    {
        code_.DUP(scratch.D2(), read(d).Delem()[d % 2U]);
    }
    void write_d(unsigned d, oaknut::QReg value)
    {
        code_.INS(write(d).Delem()[d % 2U], value.Delem()[0]);
    }
    bool used() const { return used_ != 0; }
    void restore(oaknut::VectorCodeGenerator& code) const
    {
        transfer(code, used_, true);
    }
    void restore() const { restore(code_); }
    void flush() const { transfer(code_, dirty_, false); }

private:
    static void transfer(
        oaknut::VectorCodeGenerator& code, unsigned mask, bool load)
    {
        using namespace oaknut;
        for (unsigned q = 0; q < 16; ++q) {
            if ((mask & (1U << q)) == 0)
                continue;
            const QReg reg { 16 + static_cast<int>(q) };
            const auto offset =
                offsetof(CpuThreadState, extension_registers) + q * 16U;
            if (q < 15 && (mask & (1U << (q + 1U))) != 0) {
                const QReg next { reg.index() + 1 };
                if (load)
                    code.LDP(reg, next, util::X19, offset);
                else
                    code.STP(reg, next, util::X19, offset);
                ++q;
            } else if (load)
                code.LDR(reg, util::X19, offset);
            else
                code.STR(reg, util::X19, offset);
        }
    }
    oaknut::VectorCodeGenerator& code_;
    unsigned used_ = 0, dirty_ = 0;
};
}
