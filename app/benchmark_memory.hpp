/* SPDX-License-Identifier: MPL-2.0 */
#pragma once
#include "execution/run.hpp"
#include "foundation/address_space.hpp"
#include <stdexcept>
#include <utility>

namespace ilemu {
// Benchmark commands own private resident pages. Firmware paging, shared-code
// alias domains and runtime fault delivery require a separate runtime adapter.
class BenchmarkMemory final : public execution::InstructionSource,
                              public execution::MemoryAccess {
public:
    explicit BenchmarkMemory(AddressSpace& memory)
        : memory_(memory)
    {
        memory_.set_parallel_access(false);
    }
    std::unique_ptr<execution::InstructionLease> acquire_code_lease() override
    {
        class Lease final : public execution::InstructionLease {
        public:
            Lease(AddressSpace& memory,
                std::shared_ptr<const execution::CodeIdentity> identity)
                : memory_(memory)
                , lock_(memory)
                , identity_(std::move(identity))
            {
            }
            std::shared_ptr<const execution::CodeIdentity>
            identity() const noexcept override
            {
                return identity_;
            }
            std::optional<std::uint64_t> generation() const noexcept override
            {
                return memory_.executable_content_generation();
            }

        private:
            AddressSpace& memory_;
            AddressSpace::ExclusiveAccess lock_;
            std::shared_ptr<const execution::CodeIdentity> identity_;
        };
        return std::make_unique<Lease>(memory_, identity_);
    }
    MemoryAccess* data_memory() noexcept override { return this; }
    std::optional<std::uint16_t> fetch16(std::uint32_t address) override
    {
        return memory_.read16(address, MemoryPermission::Execute);
    }
    std::optional<std::uint32_t> fetch32(std::uint32_t address) override
    {
        return memory_.read32(address, MemoryPermission::Execute);
    }
    execution::DirectMemory direct_memory() override
    {
        return { memory_.jit_read_page_table(), memory_.jit_write_page_table(),
            1, 8 };
    }
    execution::MemoryRead read(
        std::uint32_t address, execution::AccessSize size) override
    {
        std::optional<std::uint32_t> result;
        switch (size) {
        case execution::AccessSize::Byte:
            result = memory_.read8(address);
            break;
        case execution::AccessSize::Half:
            result = memory_.read16(address);
            break;
        case execution::AccessSize::Word:
            result = memory_.read32(address);
            break;
        default:
            throw std::invalid_argument("invalid benchmark access width");
        }
        return result
                   ? execution::MemoryRead { *result, { } }
                   : execution::MemoryRead { 0, fault(address, size, false) };
    }
    std::optional<execution::MemoryFault> write(std::uint32_t address,
        execution::AccessSize size, std::uint32_t value) override
    {
        bool written = false;
        switch (size) {
        case execution::AccessSize::Byte:
            written = memory_.write8(address, static_cast<std::uint8_t>(value));
            break;
        case execution::AccessSize::Half:
            written =
                memory_.write16(address, static_cast<std::uint16_t>(value));
            break;
        case execution::AccessSize::Word:
            written = memory_.write32(address, value);
            break;
        default:
            throw std::invalid_argument("invalid benchmark access width");
        }
        return written ? std::nullopt
                       : std::optional { fault(address, size, true) };
    }

private:
    execution::MemoryFault fault(
        std::uint32_t address, execution::AccessSize size, bool writing) const
    {
        return { address,
            (memory_.mapped(address, static_cast<unsigned>(size)) ? 13U : 5U) |
                (writing ? 1U << 11U : 0U) };
    }
    AddressSpace& memory_;
    std::shared_ptr<const execution::CodeIdentity> identity_ =
        std::make_shared<execution::CodeIdentity>();
};
}
