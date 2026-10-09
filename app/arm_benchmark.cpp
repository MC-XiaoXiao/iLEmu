/* SPDX-License-Identifier: MPL-2.0 */
#include "app/arm_benchmark.hpp"
#include "arm64/executor.hpp"
#include "arm_interpreter/interpreter.hpp"
#include "benchmark_memory.hpp"
#include "foundation/address_space.hpp"
#include "foundation/cpu.hpp"
#include "foundation/output.hpp"
#include "host_execution/code_memory.hpp"
#include <array>
#include <chrono>
#include <memory>
#include <stdexcept>
#include <string>

namespace ilemu {
namespace {
    void put_word(
        std::array<std::byte, 16>& code, std::size_t offset, std::uint32_t word)
    {
        for (unsigned i = 0; i < 4; ++i)
            code[offset + i] =
                std::byte { static_cast<unsigned char>(word >> (i * 8)) };
    }
}
void run_arm_benchmark(std::uint32_t iterations, std::size_t cache_size,
    std::string_view backend, Output& output)
{
    if (backend != "dynarmic" && backend != "interpreter" && backend != "arm64")
        throw std::runtime_error {
            "--executor must be dynarmic, interpreter or arm64"
        };
    AddressSpace memory;
    constexpr std::uint32_t code_address = 0x1000;
    if (!memory.map(code_address, AddressSpace::page_size,
            MemoryPermission::Read | MemoryPermission::Write |
                MemoryPermission::Execute)) {
        throw std::runtime_error { "ARM benchmark code mapping failed" };
    }
    std::array<std::byte, 16> code { };
    put_word(code, 0, 0xe3a01000U); // mov r1, #0
    put_word(code, 4, 0xe2811001U); // add r1, r1, #1
    put_word(code, 8, 0xe2500001U); // subs r0, r0, #1
    put_word(code, 12, 0x1afffffcU); // bne 0x1004
    if (!memory.copy_in(code_address, code)) {
        throw std::runtime_error { "ARM benchmark code upload failed" };
    }
    constexpr std::uint32_t svc_address = code_address + sizeof(code);
    const std::array<std::byte, 4> svc { std::byte { 0x80 }, std::byte { 0x00 },
        std::byte { 0x00 }, std::byte { 0xef } };
    if (!memory.copy_in(svc_address, svc)) {
        throw std::runtime_error { "ARM benchmark SVC upload failed" };
    }

    CpuThreadState state;
    state.registers[0] = iterations;
    state.registers[15] = code_address;
    state.cpsr = 0x10;
    const auto tick_budget = static_cast<std::uint64_t>(iterations) * 16U + 32U;
    std::uint64_t ticks = 0;
    std::optional<std::uint32_t> svc_result;
    std::chrono::steady_clock::duration elapsed;
    execution::Arm64Statistics compiled_stats;
    if (backend != "dynarmic") {
        BenchmarkMemory source { memory };
        std::unique_ptr<execution::CodeAllocator> allocator;
        std::unique_ptr<execution::Executor> executor;
        execution::Arm64Executor* compiled = nullptr;
        if (backend == "arm64") {
            if (!execution::Arm64Executor::available())
                throw std::runtime_error(
                    "ARM64 executor requires an AArch64 host");
            allocator = host::make_code_allocator();
            auto arm64 = std::make_unique<execution::Arm64Executor>(
                *allocator, cache_size);
            compiled = arm64.get();
            executor = std::move(arm64);
        } else
            executor = std::make_unique<execution::ArmInterpreter>();
        const auto started = std::chrono::steady_clock::now();
        const auto result = executor->run(state, source, { tick_budget });
        elapsed = std::chrono::steady_clock::now() - started;
        ticks = result.ticks_consumed;
        svc_result = result.svc;
        if (compiled)
            compiled_stats = compiled->statistics();
    } else {
        CpuCluster cluster { 1, memory };
        cluster.set_jit_code_cache_size(cache_size);
        auto& cpu = cluster.cpu(0);
        cpu.registers() = state.registers;
        cpu.set_cpsr(state.cpsr);
        const auto started = std::chrono::steady_clock::now();
        const auto result = cpu.run(tick_budget);
        elapsed = std::chrono::steady_clock::now() - started;
        state.registers = cpu.registers();
        ticks = result.ticks_consumed;
        svc_result = result.svc;
    }
    if (state.registers[0] != 0 || state.registers[1] != iterations ||
        svc_result != std::optional<std::uint32_t> { 0x80 }) {
        throw std::runtime_error {
            "ARM benchmark produced an unexpected CPU state"
        };
    }
    const auto elapsed_nanoseconds =
        std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count();
    const auto iterations_per_second =
        elapsed_nanoseconds > 0
            ? static_cast<std::uint64_t>(
                  static_cast<long double>(iterations) * 1'000'000'000.0L /
                  static_cast<long double>(elapsed_nanoseconds))
            : 0U;
    output.line(
        "[benchmark] baseline=arm iterations=" + std::to_string(iterations) +
        " ticks=" + std::to_string(ticks) + " elapsed-ns=" +
        std::to_string(elapsed_nanoseconds) + " jit-cache-mib=" +
        std::to_string(
            backend == "interpreter" ? 0 : cache_size / 1024U / 1024U) +
        " iterations-per-second=" + std::to_string(iterations_per_second) +
        " executor=" + std::string { backend } + " status=ok");
    if (backend == "arm64")
        output.line(
            "[arm64] compiled-regions=" +
            std::to_string(compiled_stats.compiled_regions) +
            " cache-hits=" + std::to_string(compiled_stats.cache_hits) +
            " native-calls=" + std::to_string(compiled_stats.execution_calls) +
            " compilation-ns=" +
            std::to_string(compiled_stats.compilation_nanoseconds) +
            " emitted-bytes=" + std::to_string(compiled_stats.emitted_bytes) +
            " retained-bytes=" + std::to_string(compiled_stats.retained_bytes));
}
}
