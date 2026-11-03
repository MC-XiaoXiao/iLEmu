/* SPDX-License-Identifier: MPL-2.0 */
#pragma once
#include "foundation/memory_permission.hpp"
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>

namespace ilemu {
class FilePageRead;
struct FilePageFault {
    std::shared_ptr<FilePageRead> read;
    std::uint32_t address;
    std::size_t size;
    MemoryPermission access;
};

// CPU callbacks use an abort/retry boundary; syscall copyin/out keeps its
// existing synchronous contract. No exception unwinds a native JIT frame.
class FilePageFaultScope {
public:
    FilePageFaultScope();
    ~FilePageFaultScope();
    static bool enabled() noexcept { return current_ != nullptr; }
    static void suspend(FilePageFault fault);
    [[nodiscard]] const std::optional<FilePageFault>& pending() const
    { return pending_; }

private:
    static thread_local FilePageFaultScope* current_;
    FilePageFaultScope* previous_;
    std::optional<FilePageFault> pending_;
};
}
