// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#pragma once

#include <cstdint>

namespace ilemu {

// Offsets are relative to the surface allocation, so imports retain the
// layout independently of the receiving task's virtual address.
struct SurfacePlane {
    std::uint32_t offset { };
    std::uint32_t width { };
    std::uint32_t height { };
    std::uint32_t bytes_per_row { };
    std::uint32_t size { };
    std::uint32_t bytes_per_element { 1 };
    std::uint32_t element_width { 1 };
    std::uint32_t element_height { 1 };

    [[nodiscard]] bool fits(std::uint32_t allocation_size) const
    {
        if (!width || !height || !bytes_per_row || !bytes_per_element ||
            !element_width || !element_height || offset > allocation_size)
            return false;
        const auto columns =
            (std::uint64_t { width } + element_width - 1) / element_width;
        const auto rows =
            (std::uint64_t { height } + element_height - 1) / element_height;
        const auto row_size = columns * bytes_per_element;
        const auto extent = (rows - 1) * bytes_per_row + row_size;
        return row_size <= bytes_per_row && extent <= size &&
               size <= allocation_size - offset;
    }
};

} // namespace ilemu
