/* SPDX-License-Identifier: MPL-2.0 */
#pragma once
#include "address_cache.hpp"
#include "address_displacements.hpp"
#include "address_register_plan.hpp"
#include "compiler.hpp"
#include <array>
#include <memory_resource>
#include <optional>
#include <vector>
namespace ilemu::execution::arm64 {
// Compile-only bounds. The entry selector reads GPRs, never guest memory.
// Original accesses still establish permission and pointer-lease ownership.
class AddressWindows {
public:
    using Key = AddressCache::Key;
    struct Window {
        unsigned base, alignment, natural_alignment, residue;
        bool load;
        std::int64_t first, end;
        unsigned keys = 0;
        bool indexed = false, adjusted = false, enabled = false;
        std::int32_t advance = 0;
        std::optional<unsigned> stride = {};
        std::int64_t first_coefficient = 0, last_coefficient = 0;
        std::int32_t stride_advance = 0;
        unsigned uses = 0;
    };
    explicit AddressWindows(std::pmr::memory_resource* resource)
        : accesses_(resource)
        , windows_(resource)
    {
    }
    void observe(const arm::Instruction&, const std::optional<Key>&);
    void finish(unsigned slots, unsigned cycle_instructions);
    void expand(unsigned instructions);
    std::optional<unsigned> find(const Key&) const;
    const Window& window(unsigned index) const { return windows_[index]; }
    bool empty() const;
    bool advances() const;
    AddressRegisterPlan register_plan() const;
    unsigned fallback_limit(unsigned instructions) const;
    bool can_select(const CompiledTrace& fast, const CompiledTrace& slow) const;
    void guard_group(oaknut::VectorCodeGenerator&, oaknut::Label&) const;
    CompiledTrace select(
        CompiledTrace fast, CompiledTrace slow, bool revisit = true) const;

private:
    void guard(oaknut::VectorCodeGenerator&, oaknut::Label&, bool registers) const;
    struct Access {
        Key key;
        std::int64_t first = 0, end = 0;
        unsigned residue = 0;
        bool valid = true, adjusted = false;
        std::optional<unsigned> window = { };
        std::optional<unsigned> stride = {};
        std::int64_t first_coefficient = 0, last_coefficient = 0;
        unsigned uses = 0;
    };
    std::array<std::optional<std::uint32_t>, 16> bits_ { };
    AddressDisplacements displacements_;
    AddressRegisterPlan::Builder registers_;
    std::pmr::vector<Access> accesses_;
    std::pmr::vector<Window> windows_;
    unsigned cycle_instructions_ = 0;
    std::optional<unsigned> counter_;
    bool signed_counter_ = false;
};
}
