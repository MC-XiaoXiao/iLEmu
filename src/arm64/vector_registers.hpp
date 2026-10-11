/* SPDX-License-Identifier: MPL-2.0 */
#pragma once
#include "execution/arm_state.hpp"
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <oaknut/oaknut.hpp>

namespace ilemu::execution::arm64 {
// Sixteen guest Q registers alias all thirty-two D registers. V16..V31 are
// caller-saved; V0..V2 remain scratch, V3..V7 hold immutable scalar constants,
// and host callee-saved V8..V15 are untouched. Masks describe the whole trace, including conditional
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
        // Any write may replace or alias a known scalar constant.
        for (unsigned i = 0; i < 4; ++i)
            known_constants_[(d / 2U) * 4U + i].reset();
        dirty_ |= 1U << (d / 2U);
        return read(d);
    }
    void read_d(oaknut::QReg scratch, unsigned d)
    {
        code_.DUP(scratch.D2(), read(d).Delem()[d % 2U]);
    }
    // Read-only scalar views need a copy only for an upper lane.
    oaknut::SReg scalar_s(unsigned s, oaknut::QReg scratch)
    {
        const auto bank = read(s / 2U);
        if (s % 4U == 0)
            return oaknut::SReg { bank.index() };
        code_.DUP(scratch.S4(), bank.Selem()[s % 4U]);
        return oaknut::SReg { scratch.index() };
    }
    oaknut::DReg scalar_d(unsigned d, oaknut::QReg scratch)
    {
        const auto bank = read(d);
        if (d % 2U == 0)
            return oaknut::DReg { bank.index() };
        code_.DUP(scratch.D2(), bank.Delem()[d % 2U]);
        return oaknut::DReg { scratch.index() };
    }
    void write_d(unsigned d, oaknut::QReg value)
    {
        code_.INS(write(d).Delem()[d % 2U], value.Delem()[0]);
    }
    void write_s(unsigned s, oaknut::QReg value)
    {
        code_.INS(write(s / 2U).Selem()[s % 4U], value.Selem()[0]);
    }
    bool constant_is_known(unsigned word, bool wide, std::uint8_t imm8) const
    {
        const auto& value = known_constants_[word];
        return value && value->wide == wide && value->imm8 == imm8;
    }
    void remember_constant(unsigned word, bool wide, std::uint8_t imm8)
    {
        known_constants_[word] = Constant { wide, imm8 };
    }
    oaknut::QReg constant(bool wide, std::uint8_t imm8)
    {
        for (unsigned i = 0; i < constant_count_; ++i)
            if (constants_[i].wide == wide && constants_[i].imm8 == imm8)
                return oaknut::QReg { 3 + static_cast<int>(i) };
        if (constant_count_ < constants_.size()) {
            const auto slot = constant_count_++;
            constants_[slot] = { wide, imm8 };
            return oaknut::QReg { 3 + static_cast<int>(slot) };
        }
        // A bounded plan keeps larger constant sets on the ordinary scratch path.
        materialize(code_, 0, { wide, imm8 });
        return oaknut::util::Q0;
    }
    void restore(oaknut::VectorCodeGenerator& code) const
    {
        transfer(code, used_, true);
        // Entry initialization covers conditional first uses. Host callbacks may
        // clobber all caller-saved registers, so every restore reloads constants.
        for (unsigned i = 0; i < constant_count_; ++i)
            materialize(code, 3 + static_cast<int>(i), constants_[i]);
    }
    void restore() const { restore(code_); }
    void flush() const { transfer(code_, dirty_, false); }

private:
    struct Constant {
        bool wide = false;
        std::uint8_t imm8 = 0;
    };
    static void materialize(oaknut::VectorCodeGenerator& code, int reg,
        Constant value)
    {
        if (value.wide)
            code.FMOV(oaknut::DReg { reg }, oaknut::FImm8 { value.imm8 });
        else
            code.FMOV(oaknut::SReg { reg }, oaknut::FImm8 { value.imm8 });
    }
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
    std::array<Constant, 5> constants_ { };
    unsigned constant_count_ = 0;
    // Compile-time knowledge only; every entry still emits its first write.
    std::array<std::optional<Constant>, 64> known_constants_ { };
};
}
