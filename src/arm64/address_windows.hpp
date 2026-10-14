/* SPDX-License-Identifier: MPL-2.0 */
#pragma once
#include "address_cache.hpp"
#include "address_displacements.hpp"
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
        bool unconditional = false, indexed = false, adjusted = false, enabled = false;
    };
    explicit AddressWindows(std::pmr::memory_resource* resource)
        : accesses_(resource)
        , windows_(resource)
    {
    }
    void observe(const arm::Instruction&, const std::optional<Key>&);
    void finish(unsigned slots);
    std::optional<unsigned> find(const Key&) const;
    const Window& window(unsigned index) const { return windows_[index]; }
    bool empty() const;
    CompiledTrace select(
        CompiledTrace fast, CompiledTrace slow) const;

private:
    struct Access {
        Key key;
        std::int64_t first = 0, end = 0;
        unsigned residue = 0;
        bool valid = true, unconditional = false, adjusted = false;
        std::optional<unsigned> window = { };
    };
    std::array<std::optional<std::uint32_t>, 16> bits_ { };
    AddressDisplacements displacements_;
    std::pmr::vector<Access> accesses_;
    std::pmr::vector<Window> windows_;
};
}
