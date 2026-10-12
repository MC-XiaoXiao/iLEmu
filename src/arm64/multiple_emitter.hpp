/* SPDX-License-Identifier: MPL-2.0 */
#pragma once
#include "integer_emitter.hpp"
#include "memory_exits.hpp"
namespace ilemu::execution::arm64 {
class MultipleEmitter {
public:
    MultipleEmitter(oaknut::VectorCodeGenerator& code, bool big_endian)
        : code_(code)
        , big_endian_(big_endian)
    {
    }
    void emit(const arm::Instruction&, std::uint32_t pc, std::uint64_t cost,
        MemoryExits&);

private:
    oaknut::VectorCodeGenerator& code_;
    bool big_endian_;
};
}
