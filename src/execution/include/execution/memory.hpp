/* SPDX-License-Identifier: MPL-2.0 */
#pragma once
#include <cstdint>
#include <optional>

namespace ilemu::execution {
enum class AccessSize : std::uint32_t { Byte = 1, Half = 2, Word = 4 };
struct MemoryFault {
    std::uint32_t address = 0;
    std::uint32_t status = 0;
    bool operator==(const MemoryFault&) const = default;
};
struct MemoryRead {
    std::uint32_t value = 0;
    std::optional<MemoryFault> fault;
};
// Optional A32 4 KiB page views, valid while the instruction lease is held.
// Entry + full zero-extended guest address denotes the host byte address.
// Null tables/entries use checked access. Writable entries must exclude code,
// aliases of code, overlays, COW, reservations and observable write tracking.
struct DirectMemory {
    std::uint8_t* const* read = nullptr;
    std::uint8_t* const* write = nullptr;
    std::uint32_t permits_unaligned = 0;
    std::uint32_t pc_store_offset = 8;
};
class MemoryAccess {
public:
    virtual ~MemoryAccess() = default;
    // Values are address-ordered little endian; the executor applies CPSR.E.
    // Checked operations must not partially update memory on a reported fault.
    // Host exceptions propagate at the C++ execution boundary, never through
    // generated code. Fault policy/paging belongs to the address-space adapter.
    virtual MemoryRead read(std::uint32_t, AccessSize) = 0;
    virtual std::optional<MemoryFault> write(
        std::uint32_t, AccessSize, std::uint32_t) = 0;
    virtual DirectMemory direct_memory() { return { }; }
};
}
