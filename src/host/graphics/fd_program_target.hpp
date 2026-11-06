/* SPDX-License-Identifier: MPL-2.0 */
#pragma once
#include "graphics/gles_program_target.hpp"
namespace ilemu {
// Host-only external allocations. Successful GL imports consume their FDs.
class FdProgramTarget : public GlesProgramTarget {
public:
    using GlesProgramTarget::GlesProgramTarget;
    Api api() const final { return Api::ExternalMemoryFd; }
    [[nodiscard]] virtual std::uint64_t allocation_size() const = 0;
    [[nodiscard]] virtual int export_memory_fd() const = 0;
    [[nodiscard]] virtual int export_ready_fd() const = 0;
    // Every publication gets its own binary semaphore; an earlier Vulkan
    // consumer may still be waiting when GL starts the next publication.
    [[nodiscard]] virtual int begin_publication() = 0;
    [[nodiscard]] virtual int publish() = 0;
};
}
