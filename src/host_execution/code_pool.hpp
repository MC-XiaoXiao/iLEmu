/* SPDX-License-Identifier: MPL-2.0 */
#pragma once
#include "execution/code_memory.hpp"

namespace ilemu::host::detail {
std::unique_ptr<execution::CodeAllocator> make_pooled_code_allocator(
    std::unique_ptr<execution::CodeAllocator> fallback, std::size_t page_size);
}
