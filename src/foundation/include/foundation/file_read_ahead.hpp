/* SPDX-License-Identifier: MPL-2.0 */
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>

namespace ilemu {

// Observe cold reads within one mapping. Isolated accesses stay demand-sized;
// consecutive windows grow within the pager's existing bounded ticket.
class FileReadAhead {
public:
    [[nodiscard]] std::size_t window(std::uint64_t offset,
        std::size_t demanded, std::size_t maximum) noexcept
    {
        const auto sequential = offset == next_offset_ ? previous_size_ * 2U : 0U;
        const auto size = std::min(maximum, std::max(demanded, sequential));
        next_offset_ = offset + size;
        previous_size_ = size;
        return size;
    }

private:
    std::uint64_t next_offset_ { };
    std::size_t previous_size_ { };
};

} // namespace ilemu
