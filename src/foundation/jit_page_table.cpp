// This Source Code Form is subject to the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
#include "foundation/jit_page_table.hpp"
#include <cstring>
#include <new>
#include <sys/mman.h>

namespace ilemu {
JitPageTableStorage::JitPageTableStorage()
{
    mapping_ = ::mmap(nullptr, byte_size, PROT_READ | PROT_WRITE,
        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (mapping_ == MAP_FAILED)
        throw std::bad_alloc { };
}
JitPageTableStorage::~JitPageTableStorage()
{
    ::munmap(mapping_, byte_size);
}
std::uint8_t** JitPageTableStorage::entries() const
{
    return static_cast<std::uint8_t**>(mapping_);
}
void JitPageTableStorage::clear()
{
    if (::madvise(mapping_, byte_size, MADV_DONTNEED) != 0)
        std::memset(mapping_, 0, byte_size);
}
} // namespace ilemu
