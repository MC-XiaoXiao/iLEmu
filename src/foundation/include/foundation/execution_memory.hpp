/* SPDX-License-Identifier: MPL-2.0 */
#pragma once
#include "execution/run.hpp"
#include <cstddef>
#include <memory>

namespace ilemu {
class AddressSpace;
class ArmCpuModel;

struct ExecutionMemoryPolicy {
    bool permits_unaligned = true;
    std::uint32_t pc_store_offset = 8;
};

// Runtime bridge for independent executors. Owns bounded opaque view metadata,
// never an executor or host frontend. Backing references live only in leases.
// One execution owner uses a source; other AddressSpaces may share its pages.
// Concurrent guest lanes must use AddressSpace parallel policy. Serialized
// schedulers synchronize shared-write tracking before entering any executor.
class ExecutionMemory final : public execution::InstructionSource,
                              public execution::MemoryAccess {
public:
    ExecutionMemory(AddressSpace&, const ArmCpuModel&,
        ExecutionMemoryPolicy = { }, std::size_t maximum_views = 4096);
    ~ExecutionMemory() override;
    std::unique_ptr<execution::InstructionLease> acquire_code_lease(
        std::uint32_t pc, bool thumb) override;
    execution::MemoryAccess* data_memory() noexcept override { return this; }
    std::optional<execution::MemoryFault> prepare_instruction_fetch(
        std::uint32_t, unsigned) override;
    execution::InstructionRegion instruction_region(
        std::uint32_t) const override;
    std::optional<std::uint16_t> fetch16(std::uint32_t) override;
    std::optional<std::uint32_t> fetch32(std::uint32_t) override;
    std::uint64_t ticks_for_instruction(
        std::uint32_t, std::uint32_t) const override;
    execution::DirectMemory direct_memory() override;
    execution::MemoryRead read(std::uint32_t, execution::AccessSize) override;
    std::optional<execution::MemoryFault> write(
        std::uint32_t, execution::AccessSize, std::uint32_t) override;
    [[nodiscard]] std::size_t instruction_view_count() const noexcept;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};
}
