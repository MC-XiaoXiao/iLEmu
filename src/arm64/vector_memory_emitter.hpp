/* SPDX-License-Identifier: MPL-2.0 */
#pragma once
#include "address_cache.hpp"
#include "arm/instruction.hpp"
#include "vector_registers.hpp"
#include <oaknut/oaknut.hpp>
#include <memory_resource>
#include <list>

namespace ilemu::execution::arm64 {
class VectorMemoryEmitter {
public:
    struct Split {
        Split(const arm::Instruction& value, bool big,
            std::pmr::memory_resource& labels, oaknut::Label& destination,
            oaknut::Label& failure, std::uint64_t cost, bool crosses)
            : instruction(value), big_endian(big), entry(labels), checked(labels),
              completed(destination), range(labels), failure(failure), cost(cost),
              page_split(crosses) { }
        arm::Instruction instruction;
        bool big_endian;
        oaknut::Label entry, checked;
        oaknut::Label& completed;
        oaknut::Label range;
        oaknut::Label& failure;
        std::uint64_t cost;
        bool page_split, range_used = false;
    };
    // Compile-only ownership keeps cold fragments out of the unrolled body.
    class Splits {
    public:
        explicit Splits(std::pmr::memory_resource& labels)
            : labels_(labels), paths_(&labels) { }
        Split& add(const arm::Instruction&, bool big_endian, oaknut::Label& completed,
            oaknut::Label& failure, std::uint64_t cost, bool page_split);
        void finish(oaknut::VectorCodeGenerator&, AddressCache&, VectorRegisters&);
    private:
        std::pmr::memory_resource& labels_;
        std::pmr::list<Split> paths_;
    };
    VectorMemoryEmitter(oaknut::VectorCodeGenerator& code, bool big_endian,
        AddressCache& addresses, VectorRegisters& vectors,
        std::pmr::memory_resource& labels)
        : code_(code)
        , big_endian_(big_endian)
        , addresses_(addresses)
        , vectors_(vectors)
        , labels_(labels)
    {
    }
    static std::optional<AddressCache::Key> address(const arm::Instruction&);
    static bool supports_split(const arm::Instruction&);
    bool emit_cached(const arm::Instruction&);
    void emit(const arm::Instruction&, std::uint32_t pc, std::uint64_t cost,
        oaknut::Label& checked_exit, Split* split = nullptr);

private:
    template<typename Element>
    void access_lane(bool load, Element element, oaknut::XReg pointer)
    {
        if (load)
            code_.LD1(oaknut::List { element }, pointer);
        else
            code_.ST1(oaknut::List { element }, pointer);
    }
    void access(const arm::Instruction&, AddressCache::Reference);
    bool resident_scalar(const arm::Instruction&, AddressCache::Reference);
    void scalar_multiple(const arm::Instruction&, oaknut::XReg pointer);
    void split_access(const arm::Instruction&, oaknut::XReg table,
        oaknut::Label& checked, oaknut::Label& completed);
    void endian(oaknut::QReg, unsigned size, bool quad);
    oaknut::VectorCodeGenerator& code_;
    bool big_endian_;
    AddressCache& addresses_;
    VectorRegisters& vectors_;
    std::pmr::memory_resource& labels_;
};
}
