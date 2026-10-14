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
    std::uint64_t padding = 0;
    static constexpr std::uint32_t closed = 1, ends_with_stop = 2;
    static constexpr std::uint32_t status_mask = 0x0600fe20U;
    static constexpr unsigned slot(std::uint32_t pc, std::uint32_t status)
    {
        return ((pc >> 1U) ^ (pc >> 6U) ^ (status >> 5U) ^
            (status >> 10U) ^ (status >> 25U)) & 31U;
    }
};
static_assert(sizeof(NativeContinuation) == 64);
class NativeContinuations {
public:
    struct Table {
        const void* domain = nullptr;
        std::uint64_t revision = 0, begin = 0, end = 0;
        TranslationKey profile;
        std::array<NativeContinuation, 32> entries;
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
