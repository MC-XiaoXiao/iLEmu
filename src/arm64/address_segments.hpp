/* SPDX-License-Identifier: MPL-2.0 */
#pragma once
#include "address_cache.hpp"
#include "address_displacements.hpp"
#include <memory_resource>
#include <optional>
#include <vector>

namespace ilemu::execution::arm64 {
// Compile-scoped finite ranges after a base is formed inside a recorded path.
// Every cached pointer is established by an original, checked memory access.
class AddressSegments {
public:
    struct Access {
        unsigned id;
        AddressCache::Key key;
        std::int64_t first, end;
        std::int32_t first_coefficient, last_coefficient;
        std::optional<unsigned> stride;
    };
    explicit AddressSegments(std::pmr::memory_resource* memory)
        : groups_(memory), active_(memory), accesses_(memory) { }
    void observe(unsigned step, const arm::Instruction&,
        const std::optional<AddressCache::Key>&);
    void finish(unsigned steps);
    const Access* find(unsigned step, const AddressCache::Key&) const;
    void guard(oaknut::VectorCodeGenerator&, const Access&, oaknut::Label&) const;

private:
    struct Use {
        unsigned step;
        AddressCache::Key key;
        std::int32_t constant, coefficient;
    };
    struct Group {
        AddressDisplacements offsets;
        std::pmr::vector<Use> uses;
        std::optional<unsigned> stride;
        explicit Group(std::pmr::memory_resource* memory) : uses(memory) { }
    };
    std::pmr::vector<Group> groups_;
    std::pmr::vector<unsigned> active_;
    std::pmr::vector<std::optional<Access>> accesses_;
};
}
