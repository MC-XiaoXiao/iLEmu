/* SPDX-License-Identifier: MPL-2.0 */
#pragma once
#include "compiler.hpp"
#include <vector>
#include <cstddef>
#include <oaknut/oaknut.hpp>
namespace ilemu::execution::arm64 {
// Fixed native ABI transfers contain no guest addresses, policy, or branches.
// Assemble each immutable sequence once with Oaknut, then append it in bulk.
class NativeFrame {
public:
    // D8/D9 belong to one invocation-local memory-cache owner at a time.
    static constexpr std::ptrdiff_t memory_cache_offset = 176;
    static void entry(std::vector<std::uint32_t>& words)
    {
        static const auto sequence = [] {
            std::vector<std::uint32_t> result;
            oaknut::VectorCodeGenerator code(result);
            emit_entry(code);
            return result;
        }();
        words.insert(words.end(), sequence.begin(), sequence.end());
    }
    static void publish(std::vector<std::uint32_t>& words)
    {
        static const auto sequence = [] {
            std::vector<std::uint32_t> result;
            oaknut::VectorCodeGenerator code(result);
            emit_publish(code);
            return result;
        }();
        words.insert(words.end(), sequence.begin(), sequence.end());
    }
    static void leave(std::vector<std::uint32_t>& words)
    {
        static const auto sequence = [] {
            std::vector<std::uint32_t> result;
            oaknut::VectorCodeGenerator code(result);
            emit_leave(code);
            return result;
        }();
        words.insert(words.end(), sequence.begin(), sequence.end());
    }
    static void tail(std::vector<std::uint32_t>& words)
    {
        oaknut::VectorCodeGenerator code(words);
        emit_leave(code, true);
    }
private:
    template<typename Code> static void emit_entry(Code& code)
    {
        using namespace oaknut;
        using namespace oaknut::util;
        code.STP(X19, X20, SP, PreIndexed { }, -192);
        code.STP(X21, X22, SP, 16);
        code.STP(X23, X24, SP, 32);
        code.STP(X25, X26, SP, 48);
        code.STP(X27, X28, SP, 64);
        code.STP(X29, X30, SP, 80);
        code.MOV(X19, X0);
        code.MOV(X20, X1);
        code.LDP(X25, X26, X20, offsetof(NativeOutcome, memory));
        code.LDR(X23, X20, offsetof(NativeOutcome, groups));
        code.MOV(X24, 0);
        // LR is saved in the frame. Between control calls X30 counts
        // groups until the next poll; BLR overwrites it only at a poll.
        code.MOV(X30, 1);
        code.LDR(W22, X19, offsetof(CpuThreadState, cpsr));
        code.MSR(SystemReg::NZCV, X22);
        for (int i = 0; i < 14; i += 2)
            code.LDP(WReg { i }, WReg { i + 1 }, X19, i * 4);
        code.LDR(W14, X19, 56);
        code.align(16);
    }
    template<typename Code> static void emit_publish(Code& code)
    {
        using namespace oaknut;
        using namespace oaknut::util;
        for (int i = 0; i < 14; i += 2)
            code.STP(WReg { i }, WReg { i + 1 }, X19, i * 4);
        code.STR(W14, X19, 56);
        code.MRS(X21, SystemReg::NZCV);
        code.AND(W22, W22, 0x0fffffffU);
        code.ORR(W22, W22, W21);
        code.STR(W22, X19, offsetof(CpuThreadState, cpsr));
    }
    template<typename Code> static void emit_leave(Code& code, bool tail = false)
    {
        using namespace oaknut;
        using namespace oaknut::util;
        code.LDP(X21, X22, SP, 16);
        code.LDP(X23, X24, SP, 32);
        code.LDP(X25, X26, SP, 48);
        code.LDP(X27, X28, SP, 64);
        code.LDP(X29, X30, SP, 80);
        code.LDP(X19, X20, SP, PostIndexed { }, 192);
        if (tail)
            code.BR(oaknut::XReg { 16 });
        else
            code.RET();
    }
};
}
