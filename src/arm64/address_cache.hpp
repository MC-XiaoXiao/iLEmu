/* SPDX-License-Identifier: MPL-2.0 */
#pragma once
#include "arm/instruction.hpp"
#include <array>
#include <cstddef>
#include <cstdint>
#include <oaknut/oaknut.hpp>
#include <optional>
#include <vector>
#include <variant>

namespace ilemu::execution::arm64 {
class AddressWindows;
// Dominating guards publish into X27/X28 and optional D8/D9 slots. Only invariant
// slots survive a closed trace backedge; no pointer survives a native
// invocation or checked exit. Callers revoke entries on every architectural
// address input write.
class AddressCache {
public:
    struct Key {
        unsigned base, size;
        std::uint32_t offset;
        bool add, load;
        unsigned alignment, natural_alignment;
        std::optional<unsigned> index = { };
        arm::ShiftKind shift = arm::ShiftKind::Lsl;
        unsigned shift_amount = 0;
        // An architectural PC base is constant at this instruction, whereas
        // the offset register remains an ordinary invalidation dependency.
        std::optional<std::uint32_t> pc_base = { };
        std::uint32_t inputs() const
        {
            return (pc_base ? 0U : 1U << base) | (index ? 1U << *index : 0U);
        }
        bool operator==(const Key&) const = default;
    };
    struct Reference {
        oaknut::XReg base;
        std::int32_t offset = 0;
        bool indexed = false;
        oaknut::XReg immediate(oaknut::VectorCodeGenerator&) const;
    };
    // Extra slots share D8/D9 with ReadPageCache, so the caller grants them
    // only when that dynamic page resolver is disabled for this trace.
    AddressCache(oaknut::VectorCodeGenerator&, bool extra_slots_available,
        const AddressWindows* = nullptr);
    std::optional<Reference> find(const Key&) const;
    void begin_guard(const Key&);
    Reference remember(const Key&, oaknut::XReg pointer);
    void invalidate(unsigned reg);
    void clear();
    void enter(oaknut::VectorCodeGenerator&) const;
    void leave() const;
    void finalize(bool closed, std::vector<std::uint32_t>&,
        oaknut::VectorCodeGenerator& entry) const;

private:
    using Identity = std::variant<Key, unsigned>;
    Identity identity(const Key&) const;
    std::uint32_t inputs(const Identity&) const;
    Reference reference(const Key&, const Identity&, oaknut::XReg) const;
    struct Guard {
        Identity identity;
        std::ptrdiff_t branch, target;
    };
    oaknut::VectorCodeGenerator& code_;
    std::array<std::optional<Identity>, 4> entries_;
    std::array<std::optional<Guard>, 4> first_;
    const AddressWindows* windows_;
    unsigned slot_count_, next_ = 0, written_ = 0;
    bool used_extra_ = false;
};
}
