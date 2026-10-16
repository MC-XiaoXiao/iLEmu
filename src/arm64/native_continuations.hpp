/* SPDX-License-Identifier: MPL-2.0 */
#pragma once
#include "execution/code_memory.hpp"
#include "compilation_key.hpp"
#include <array>
#include <cstdint>
namespace ilemu::execution::arm64 {
// Plain generated-code ABI. Owners and executable allocations remain borrowed
// only during the code lease; the cache cannot mutate during native execution.
struct NativeContinuation {
    const void* entry = nullptr;
    std::uint64_t maximum_ticks = 0;
    void* owner = nullptr;
    std::uint64_t visits = 0, heat = 0;
    std::uint32_t pc = 0, status = 0, flags = 0, reserved = 0;
    const std::uint64_t* validation = nullptr;
    static constexpr std::uint32_t closed = 1, ends_with_stop = 2;
    static constexpr std::uint32_t status_mask = 0x0600fe20U;
    static constexpr unsigned set_count = 32, ways = 2;
    static constexpr unsigned slot(std::uint32_t pc, std::uint32_t status)
    {
        return ((pc >> 1U) ^ (pc >> 6U) ^ (status >> 5U) ^
            (status >> 10U) ^ (status >> 25U)) & (set_count - 1U);
    }
};
static_assert(sizeof(NativeContinuation) == 64);
class NativeContinuations {
public:
    // Independent of table capacity: every edge polls stops, and paid guest
    // ticks bound the whole invocation even when its fragments are tiny.
    static constexpr std::uint64_t maximum_ticks = 32768;
    // Tiny fragments must not exhaust hops before the paid-tick bound.
    static constexpr unsigned maximum_hops = static_cast<unsigned>(maximum_ticks);
    struct Table {
        const void* domain = nullptr;
        std::uint64_t revision = 0, begin = 0, end = 0;
        TranslationKey profile;
        struct View {
            const void* domain = nullptr;
            std::uint64_t begin = 0, end = 0, cookie = 0;
        };
        // Validation belongs to a region as well as its opaque identity: a
        // custom source may use one identity for many separately pinned views.
        std::array<View, 6> views;
        // Keep the primary lookup unchanged. A second way holds conflicting
        // fragments without another lease lookup on every alternating edge.
        std::array<NativeContinuation, NativeContinuation::set_count *
            NativeContinuation::ways> entries;
        std::uint32_t replacement_mask = 0;
        NativeContinuation& select(std::uint32_t pc, std::uint32_t status)
        {
            const auto first = NativeContinuation::slot(pc, status);
            for (unsigned way = 0; way < NativeContinuation::ways; ++way) {
                auto& entry = entries[first + way * NativeContinuation::set_count];
                if (entry.entry && entry.pc == pc && entry.status == status)
                    return entry;
            }
            for (unsigned way = 0; way < NativeContinuation::ways; ++way) {
                auto& entry = entries[first + way * NativeContinuation::set_count];
                if (!entry.entry)
                    return entry;
            }
            const auto way = (replacement_mask >> first) & 1U;
            replacement_mask ^= 1U << first;
            return entries[first + way * NativeContinuation::set_count];
        }
    };
    explicit NativeContinuations(CodeAllocator&);
    std::size_t retained_bytes() const { return sizeof(*this) + dispatcher_->capacity(); }
    const void* dispatcher() const { return dispatcher_->entry(); }
    std::array<Table, 2> tables;
    unsigned replacement = 0;
private:
    std::unique_ptr<ExecutableCode> dispatcher_;
};
}
