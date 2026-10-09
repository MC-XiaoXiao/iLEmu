/* SPDX-License-Identifier: MPL-2.0 */
#pragma once
#include "execution/code_memory.hpp"
#include "execution/run.hpp"
#include <cstddef>
#include <memory>
namespace ilemu::execution {
struct Arm64Statistics {
    std::uint64_t compiled_regions = 0;
    std::uint64_t cache_hits = 0;
    std::uint64_t execution_calls = 0;
    std::uint64_t emitted_bytes = 0;
    std::uint64_t compilation_nanoseconds = 0;
    std::size_t retained_bytes = 0;
};
class Arm64Executor final : public Executor {
public:
    explicit Arm64Executor(
        CodeAllocator&, std::size_t cache_budget = 8U * 1024U * 1024U);
    ~Arm64Executor() override;
    static bool available() noexcept;
    RunResult run(
        CpuThreadState&, InstructionSource&, const RunRequest&) override;
    void request_stop(StopReason) noexcept override;
    // Owner-only, with no native invocation active.
    void clear_stop() noexcept override;
    void clear_cache() override;
    std::uint64_t retained_code_bytes() const noexcept override;
    std::uint64_t translation_count() const noexcept override;
    Arm64Statistics statistics() const noexcept;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};
}
