// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#pragma once
#include <foundation/address_space.hpp>
#include <kernel/darwin_abi.hpp>
#include <network/darwin_abi_route.hpp>
#include <network/darwin_socket_abi.hpp>
#include <algorithm>
#include <cstdint>
#include <span>
#include <vector>

namespace ilemu {
struct GuestReadVector {
    std::uint32_t address;
    std::uint32_t length;
};

// A scalar target stays allocation-free. Imported vectors are owned by the
// syscall until completion, or moved into its existing pending read record.
class GuestReadBuffer {
public:
    GuestReadBuffer(std::uint32_t address,
        std::span<const GuestReadVector> vectors = {})
        : address_(address), vectors_(vectors) {}

    static std::uint32_t import(AddressSpace& memory, std::uint32_t address,
        std::uint32_t count, DarwinAbiEpoch epoch,
        std::vector<GuestReadVector>& vectors, std::uint64_t& residual)
    {
        // sys_generic.c:readv; kern_subr.c:copyin_user_iovec_array.
        if (count == 0 || count > darwin::io::maximum_vector_count)
            return 22; // EINVAL
        if (address == 0 || !memory.accessible(address,
                count * darwin::socket::arm32_iovec::size, MemoryPermission::Read))
            return 14; // EFAULT
        vectors.reserve(count);
        residual = 0;
        for (std::uint32_t i = 0; i < count; ++i) {
            const auto entry = address + i * darwin::socket::arm32_iovec::size;
            const auto base = memory.read32(entry + darwin::socket::arm32_iovec::base_offset);
            const auto length = memory.read32(entry + darwin::socket::arm32_iovec::length_offset);
            if (!base || !length)
                return 14;
            vectors.push_back({*base, *length});
            // Native uio_calculateresid excludes null bases. The vector
            // remains present: a later nonzero residual still faults on it.
            if (*base != 0)
                residual += *length;
        }
        // xnu-2422+ rejects residuals beyond a native ARM32 signed long.
        if (epoch == DarwinAbiEpoch::Later && residual > 0x7fffffffULL)
            return 22;
        return 0;
    }

    bool copy(AddressSpace& memory, std::span<const std::byte> bytes,
        std::size_t* transferred = nullptr) const
    {
        std::size_t copied = 0;
        const auto finish = [&](bool result) {
            if (transferred) *transferred = copied;
            return result;
        };
        if (vectors_.empty()) {
            const auto ok = memory.copy_in(address_, bytes);
            if (ok) copied = bytes.size();
            return finish(ok);
        }
        if (bytes.empty()) return finish(true);
        for (const auto& vector : vectors_) {
            const auto count = std::min<std::size_t>(vector.length, bytes.size() - copied);
            if (count && memory.accessible(vector.address, count, MemoryPermission::Write)) {
                if (!memory.copy_in(vector.address, bytes.subspan(copied, count)))
                    return finish(false);
                copied += count;
            } else {
                // AddressSpace::copy_in is a loader/internal write primitive.
                // Check guest protection and preserve a prefix before EFAULT.
                std::size_t offset = 0;
                while (offset < count) {
                    const auto address = static_cast<std::uint64_t>(vector.address) + offset;
                    if (address >= (std::uint64_t {1} << 32U)) return finish(false);
                    const auto current = static_cast<std::uint32_t>(address);
                    const auto chunk = std::min<std::size_t>(count - offset,
                        AddressSpace::page_size - (current & (AddressSpace::page_size - 1U)));
                    if (!memory.accessible(current, chunk, MemoryPermission::Write) ||
                        !memory.copy_in(current, bytes.subspan(copied, chunk)))
                        return finish(false);
                    offset += chunk;
                    copied += chunk;
                }
            }
            if (copied == bytes.size()) return finish(true);
        }
        return finish(false);
    }
private:
    std::uint32_t address_;
    std::span<const GuestReadVector> vectors_;
};
} // namespace ilemu
