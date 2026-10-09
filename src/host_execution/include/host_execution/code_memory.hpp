/* SPDX-License-Identifier: MPL-2.0 */
#pragma once
#include "execution/code_memory.hpp"
namespace ilemu::host {
// The current implementation supports Linux/Android W^X publication.
std::unique_ptr<execution::CodeAllocator> make_code_allocator();
}
