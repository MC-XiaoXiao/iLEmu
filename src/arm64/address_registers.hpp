/* SPDX-License-Identifier: MPL-2.0 */
#pragma once
#include "address_register_plan.hpp"
#include <oaknut/oaknut.hpp>
#include <cstdint>
#include <optional>
namespace ilemu::execution::arm64 {
// A guarded group keeps address-only bases as page pointers or unchanged entry
// values with fixed offsets. Every exit publishes architectural guest values.
class AddressRegisters {
public:
    using Offsets = std::array<std::uint32_t, AddressRegisterPlan::capacity>;
    AddressRegisters(oaknut::VectorCodeGenerator&, const AddressRegisterPlan&);
    bool eligible(unsigned reg) const;
    bool active(unsigned reg) const { return (active_ & (1U << reg)) != 0; }
    unsigned mask() const { return active_; }
    bool entry_relative(unsigned reg) const
    {
        return (plan_.deferred_mask & (1U << reg)) != 0;
    }
    const Offsets& deferred_offsets() const { return offsets_; }
    void acquire(unsigned reg, oaknut::XReg bias);
    void scope(unsigned allowed, unsigned ending);
    bool update(const arm::Instruction&);
    bool writeback(const arm::Instruction&);
    void read_guest(oaknut::WReg destination, unsigned reg) const;
    void restore(unsigned mask) const;
    void publish_offsets(const Offsets&) const;
    void finish_group();
private:
    std::optional<unsigned> slot(unsigned reg) const;
    void add_offset(oaknut::WReg destination, oaknut::WReg base,
        std::uint32_t offset) const;
    oaknut::VectorCodeGenerator& code_;
    AddressRegisterPlan plan_;
    unsigned active_ = 0;
    unsigned allowed_ = ~0U;
    Offsets offsets_ { };
};
}
