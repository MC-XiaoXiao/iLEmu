/* SPDX-License-Identifier: MPL-2.0 */
#pragma once
#include "execution/arm_state.hpp"
#include "simd_immediate_emitter.hpp"
#include "scalar_register_plan.hpp"
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <oaknut/oaknut.hpp>

namespace ilemu::execution::arm64 {
// Sixteen guest Q registers alias all thirty-two D registers. V16..V31 are
// caller-saved; V0..V2 remain scratch, V3..V7 share constants and scalar views.
// A whole-cycle scalar plan may also reuse untouched guest-bank registers,
// and host V8/V9 are reserved for invocation-local memory caches. Other
// callee-saved vectors are untouched. Masks describe the whole trace, including conditional
// paths. Entry loads preserve neighbours even for a write-only D or lane
// destination.
class VectorRegisters {
public:
    explicit VectorRegisters(oaknut::VectorCodeGenerator& code,
        const ScalarRegisterPlan* scalars = nullptr)
        : code_(code), scalars_(scalars)
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
        if (auto resident = resident_d(d))
            code_.FMOV(scratch.toD(), *resident);
        else if (auto pair = resident_pair(d)) {
            code_.FMOV(scratch.toS(), pair->first);
            code_.INS(scratch.Selem()[1], oaknut::QReg { pair->second.index() }.Selem()[0]);
        } else
            code_.DUP(scratch.D2(), read(d).Delem()[d % 2U]);
    }
    // Read-only scalar views need a copy only for an upper lane.
    oaknut::SReg scalar_s(unsigned s, oaknut::QReg scratch)
    {
        if (auto resident = resident_s(s))
            return *resident;
        if (auto wide = resident_d(s / 2U)) {
            if (s % 2U == 0)
                return oaknut::SReg { wide->index() };
            code_.DUP(scratch.S4(), oaknut::QReg { wide->index() }.Selem()[1]);
            return oaknut::SReg { scratch.index() };
        }
        const auto bank = read(s / 2U);
        if (s % 4U == 0)
            return oaknut::SReg { bank.index() };
        code_.DUP(scratch.S4(), bank.Selem()[s % 4U]);
        return oaknut::SReg { scratch.index() };
    }
    oaknut::DReg scalar_d(unsigned d, oaknut::QReg scratch)
    {
        if (auto resident = resident_d(d))
            return *resident;
        if (resident_pair(d)) {
            read_d(scratch, d);
            return oaknut::DReg { scratch.index() };
        }
        const auto bank = read(d);
        if (d % 2U == 0)
            return oaknut::DReg { bank.index() };
        code_.DUP(scratch.D2(), bank.Delem()[d % 2U]);
        return oaknut::DReg { scratch.index() };
    }
    std::optional<oaknut::SReg> resident_s(unsigned s) const
    {
        if (scalars_)
            if (auto slot = scalars_->find(s, false))
                return oaknut::SReg { static_cast<int>(scalars_->entries[*slot].host) };
        return {};
    }
    std::optional<oaknut::DReg> resident_d(unsigned d) const
    {
        if (scalars_)
            if (auto slot = scalars_->find(d * 2U, true))
                return oaknut::DReg { static_cast<int>(scalars_->entries[*slot].host) };
        return {};
    }
    std::optional<std::pair<oaknut::SReg, oaknut::SReg>> resident_pair(unsigned d) const
    {
        const auto low = resident_s(d * 2U), high = resident_s(d * 2U + 1U);
        if (low && high)
            return std::pair { *low, *high };
        return {};
    }
    void write_d(unsigned d, oaknut::QReg value)
    {
        if (auto resident = resident_d(d))
            code_.FMOV(*resident, oaknut::DReg { value.index() });
        else if (auto pair = resident_pair(d)) {
            code_.FMOV(pair->first, oaknut::SReg { value.index() });
            code_.DUP(oaknut::QReg { pair->second.index() }.S4(), value.Selem()[1]);
        } else
            code_.INS(write(d).Delem()[d % 2U], value.Delem()[0]);
    }
    void write_s(unsigned s, oaknut::QReg value)
    {
        if (auto resident = resident_s(s))
            code_.FMOV(*resident, oaknut::SReg { value.index() });
        else if (auto wide = resident_d(s / 2U))
            code_.INS(oaknut::QReg { wide->index() }.Selem()[s % 2U], value.Selem()[0]);
        else
            code_.INS(write(s / 2U).Selem()[s % 4U], value.Selem()[0]);
    }
    bool constant_is_known(unsigned word, bool wide, std::uint8_t imm8) const
    {
        const auto& value = known_constants_[word];
        return value && value->wide == wide && value->imm8 == imm8;
    }
    void remember_constant(unsigned word, bool wide, std::uint8_t imm8)
    {
        known_constants_[word] = ScalarConstant { wide, imm8 };
    }
    oaknut::QReg constant(bool wide, std::uint8_t imm8)
    {
        for (unsigned i = 0; i < constant_count_; ++i)
            if (!constants_[i].vector && constants_[i].wide == wide &&
                constants_[i].imm8 == imm8)
                return oaknut::QReg { 3 + static_cast<int>(constant_slots() + i) };
        if (constant_slots() + constant_count_ < constants_.size()) {
            const auto slot = constant_count_++;
            constants_[slot] = { wide, imm8 };
            return oaknut::QReg { 3 + static_cast<int>(constant_slots() + slot) };
        }
        // A bounded plan keeps larger constant sets on the ordinary scratch path.
        materialize(code_, 0, { wide, imm8 });
        return oaknut::util::Q0;
    }
    oaknut::QReg simd_constant(
        const arm::VectorImmediateOperands& value, bool upper)
    {
        const Constant requested { false, value.imm8, true, value.cmode,
            value.op, upper };
        for (unsigned i = 0; i < constant_count_; ++i) {
            const auto& entry = constants_[i];
            if (entry.vector && entry.imm8 == value.imm8 &&
                entry.cmode == value.cmode && entry.op == value.op &&
                entry.upper == upper)
                return oaknut::QReg { 3 + static_cast<int>(constant_slots() + i) };
        }
        if (constant_slots() + constant_count_ < constants_.size()) {
            const auto slot = constant_count_++;
            constants_[slot] = requested;
            return oaknut::QReg { 3 + static_cast<int>(constant_slots() + slot) };
        }
        materialize(code_, 0, requested);
        return oaknut::util::Q0;
    }
    void restore(oaknut::VectorCodeGenerator& code) const
    {
        transfer(code, used_, true);
        // Entry initialization covers conditional first uses. Host callbacks may
        // clobber all caller-saved registers, so every restore reloads constants.
        for (unsigned i = 0; i < constant_count_; ++i)
            materialize(code, 3 + static_cast<int>(constant_slots() + i), constants_[i]);
        scalar_transfer(code, true);
    }
    void restore() const { restore(code_); }
    void flush() const
    {
        // Packed neighbours may be stale. Resident writes override only their
        // own words after the ordinary bank publication, on every exit path.
        transfer(code_, dirty_, false);
        scalar_transfer(code_, false);
    }

private:
    unsigned constant_slots() const
    {
        return scalars_ ? scalars_->constant_slots() : 0U;
    }
    void scalar_transfer(oaknut::VectorCodeGenerator& code, bool load) const
    {
        if (!scalars_)
            return;
        for (unsigned i = 0; i < scalars_->count; ++i) {
            const auto& entry = scalars_->entries[i];
            if (!load && !entry.written)
                continue;
            const auto offset = static_cast<std::ptrdiff_t>(
                offsetof(CpuThreadState, extension_registers) +
                entry.word * sizeof(std::uint32_t));
            const auto emit = [&](auto reg) {
                if (load)
                    code.LDR(reg, oaknut::util::X19, offset);
                else
                    code.STR(reg, oaknut::util::X19, offset);
            };
            if (entry.wide)
                emit(oaknut::DReg { static_cast<int>(entry.host) });
            else
                emit(oaknut::SReg { static_cast<int>(entry.host) });
        }
    }
    struct ScalarConstant {
        bool wide;
        std::uint8_t imm8;
    };
    struct Constant {
        bool wide = false;
        std::uint8_t imm8 = 0;
        bool vector = false;
        std::uint8_t cmode = 0;
        bool op = false, upper = false;
    };
    static void materialize(oaknut::VectorCodeGenerator& code, int reg,
        Constant value)
    {
        if (value.vector) {
            const oaknut::QReg destination { reg };
            emit_simd_immediate(code, destination,
                { 0, value.imm8, value.cmode, value.op, true });
            // A scalar write clears the other half; rotating places a mask
            // in the upper D without touching the neighbouring guest lane.
            code.FMOV(destination.toD(), destination.toD());
            if (value.upper)
                code.EXT(destination.B16(), destination.B16(),
                    destination.B16(), 8);
        } else if (value.wide)
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
    // Borrowed only during compilation; no plan pointer enters native code.
    const ScalarRegisterPlan* scalars_;
    unsigned used_ = 0, dirty_ = 0;
    std::array<Constant, ScalarRegisterPlan::constant_capacity> constants_ { };
    unsigned constant_count_ = 0;
    // Compile-time knowledge only; every entry still emits its first write.
    std::array<std::optional<ScalarConstant>, 64> known_constants_ { };
};
}
