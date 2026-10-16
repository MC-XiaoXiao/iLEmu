/* SPDX-License-Identifier: MPL-2.0 */
#include "address_registers.hpp"
#include "address_displacements.hpp"
#include "compiler.hpp"
#include <cstddef>
#include <stdexcept>
namespace ilemu::execution::arm64 {
using namespace oaknut;
using namespace oaknut::util;
namespace {
    constexpr auto biases = offsetof(NativeOutcome, address_biases);
    static_assert(std::tuple_size_v<decltype(NativeOutcome::address_biases)> ==
        AddressRegisterPlan::capacity);
}
AddressRegisters::AddressRegisters(
    VectorCodeGenerator& code, const AddressRegisterPlan& plan)
    : code_(code), plan_(plan) { }
std::optional<unsigned> AddressRegisters::slot(unsigned reg) const
{
    for (unsigned i = 0; i < plan_.count; ++i)
        if (plan_.bases[i] == reg)
            return i;
    return {};
}
bool AddressRegisters::eligible(unsigned reg) const { return slot(reg).has_value(); }
void AddressRegisters::acquire(unsigned reg, XReg bias)
{
    if (active(reg))
        return;
    const auto index = slot(reg).value();
    code_.STR(bias, X20, biases + index * sizeof(std::uint64_t));
    const auto base = XReg { static_cast<int>(reg) };
    code_.ADD(base, bias, base);
    active_ |= 1U << reg;
}
bool AddressRegisters::update(const arm::Instruction& inst)
{
    if ((inst.opcode >= 8 && inst.opcode <= 11) || !active(inst.rd))
        return false;
    if (!AddressRegisterPlan::update(inst))
        throw std::logic_error("invalid native address update");
    const auto base = XReg { static_cast<int>(inst.rd) };
    if (inst.immediate_operand) {
        if (inst.opcode == 4)
            code_.ADD(base, base, inst.immediate);
        else
            code_.SUB(base, base, inst.immediate);
    } else {
        // Window selection interprets the invariant stride as signed32. Its
        // bounds prove all accessed addresses remain in the resolved page.
        const auto stride = WReg { static_cast<int>(inst.rm) };
        if (inst.opcode == 4)
            code_.ADD(base, base, stride, AddSubExt::SXTW, inst.shift_amount);
        else
            code_.SUB(base, base, stride, AddSubExt::SXTW, inst.shift_amount);
    }
    return true;
}
bool AddressRegisters::writeback(const arm::Instruction& inst)
{
    if (!active(inst.rn))
        return false;
    const auto change = AddressDisplacements::writeback(inst);
    if (!change || change->delta < -4095 || change->delta > 4095)
        throw std::logic_error("invalid native address writeback");
    const auto base = XReg { static_cast<int>(inst.rn) };
    if (change->delta < 0)
        code_.SUB(base, base, static_cast<unsigned>(-change->delta));
    else
        code_.ADD(base, base, static_cast<unsigned>(change->delta));
    return true;
}
void AddressRegisters::read_guest(WReg destination, unsigned reg) const
{
    const auto base = WReg { static_cast<int>(reg) };
    if (!active(reg)) {
        code_.MOV(destination, base);
        return;
    }
    code_.LDR(X17, X20, biases + slot(reg).value() * sizeof(std::uint64_t));
    code_.SUB(destination, base, W17);
}
void AddressRegisters::restore(unsigned mask) const
{
    for (unsigned i = 0; i < plan_.count; ++i) {
        const auto reg = plan_.bases[i];
        if ((mask & (1U << reg)) == 0)
            continue;
        code_.LDR(X17, X20, biases + i * sizeof(std::uint64_t));
        const auto base = WReg { static_cast<int>(reg) };
        code_.SUB(base, base, W17);
    }
}
void AddressRegisters::finish_group()
{
    // Group guards and control polls use architectural W registers. Re-enter
    // through the ordinary first publication to acquire pointers again.
    restore(active_);
    active_ = 0;
}
}
