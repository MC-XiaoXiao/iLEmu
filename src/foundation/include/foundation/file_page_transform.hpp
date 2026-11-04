/* SPDX-License-Identifier: MPL-2.0 */
#pragma once

#include <cstdint>
#include <memory>

namespace ilemu {
class FilePageCache;
struct GuestPageBacking;

// An immutable loader recipe applied before a private file page becomes
// visible to Guest accesses or JIT tables. Return the source for an unchanged
// page, an immutable replacement, or null to fail the page fault.
class FilePageTransform {
public:
    virtual ~FilePageTransform() = default;
    [[nodiscard]] virtual std::shared_ptr<GuestPageBacking> apply(
        FilePageCache& cache, std::shared_ptr<GuestPageBacking> source,
        std::uint64_t file_offset) const = 0;
};
} // namespace ilemu
