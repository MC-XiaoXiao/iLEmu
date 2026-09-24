// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// ABI: apple-oss-distributions/dyld, launch-cache/dyld_cache_format.h.
#include "dyld_cache_local_symbols.hpp"

#include <string_view>

namespace ilemu {
namespace {
    std::uint32_t u32(std::span<const std::byte> bytes, std::size_t offset)
    {
        return std::to_integer<std::uint32_t>(bytes[offset]) |
               (std::to_integer<std::uint32_t>(bytes[offset + 1]) << 8U) |
               (std::to_integer<std::uint32_t>(bytes[offset + 2]) << 16U) |
               (std::to_integer<std::uint32_t>(bytes[offset + 3]) << 24U);
    }

    std::uint64_t u64(std::span<const std::byte> bytes, std::size_t offset)
    {
        return u32(bytes, offset) |
               (static_cast<std::uint64_t>(u32(bytes, offset + 4U)) << 32U);
    }

    bool fits(std::span<const std::byte> bytes, std::uint64_t offset,
        std::uint64_t count, std::uint64_t stride = 1U)
    {
        return offset <= bytes.size() &&
               count <= (bytes.size() - offset) / stride;
    }
} // namespace

std::optional<Arm32DyldCacheLocalSymbols> Arm32DyldCacheLocalSymbols::find(
    std::span<const std::byte> cache, std::uint64_t image_offset)
{
    // mappingOffset bounds the available header fields. The old image table
    // selects the embedded 32-bit dylib-offset ABI, not newer symbol subcaches.
    if (cache.size() < 88U ||
        std::string_view { reinterpret_cast<const char*>(cache.data()), 7U } !=
            "dyld_v1" ||
        u32(cache, 16U) < 88U || u32(cache, 16U) > cache.size() ||
        u32(cache, 24U) == 0U || u32(cache, 28U) == 0U)
        return std::nullopt;
    const auto offset = u64(cache, 72U);
    const auto size = u64(cache, 80U);
    if (offset == 0U || size < 24U || !fits(cache, offset, size))
        return std::nullopt;
    const auto chunk = cache.subspan(offset, size);
    const auto nlist_offset = u32(chunk, 0U);
    const auto nlist_count = u32(chunk, 4U);
    const auto strings_offset = u32(chunk, 8U);
    const auto strings_size = u32(chunk, 12U);
    const auto entries_offset = u32(chunk, 16U);
    const auto entries_count = u32(chunk, 20U);
    if (!fits(chunk, nlist_offset, nlist_count, 12U) ||
        !fits(chunk, strings_offset, strings_size) ||
        !fits(chunk, entries_offset, entries_count, 12U))
        return std::nullopt;
    for (std::uint64_t index = 0; index < entries_count; ++index) {
        const auto entry = entries_offset + index * 12U;
        if (u32(chunk, entry) != image_offset)
            continue;
        const auto start = u32(chunk, entry + 4U);
        const auto count = u32(chunk, entry + 8U);
        if (start > nlist_count || count > nlist_count - start)
            return std::nullopt;
        return Arm32DyldCacheLocalSymbols {
            chunk.subspan(
                nlist_offset + static_cast<std::uint64_t>(start) * 12U,
                static_cast<std::uint64_t>(count) * 12U),
            chunk.subspan(strings_offset, strings_size)
        };
    }
    return std::nullopt;
}

} // namespace ilemu
