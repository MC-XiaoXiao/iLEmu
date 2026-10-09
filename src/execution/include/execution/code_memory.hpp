/* SPDX-License-Identifier: MPL-2.0 */
#pragma once
#include <cstddef>
#include <memory>
#include <span>

namespace ilemu::execution {
// Host allocation/protection/icache operations stay outside the guest backend.
// Published allocations are immutable and remain alive during execution.
class ExecutableCode {
public:
    virtual ~ExecutableCode() = default;
    virtual std::size_t capacity() const noexcept = 0;
    virtual const void* entry() const noexcept = 0;
    virtual void publish(std::span<const std::byte>) = 0;
};
class CodeAllocator {
public:
    virtual ~CodeAllocator() = default;
    virtual std::unique_ptr<ExecutableCode> allocate(std::size_t bytes) = 0;
};
}
