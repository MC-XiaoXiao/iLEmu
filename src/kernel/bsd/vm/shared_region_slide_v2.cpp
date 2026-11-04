// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// XNU's vm_shared_region_slide_page_v2 wire format and pointer-chain rules:
// https://github.com/apple-oss-distributions/xnu/blob/xnu-3789.1.32/osfmk/vm/vm_shared_region.c

#include "shared_region_slide_v2.hpp"

#include "foundation/address_space.hpp"
#include "foundation/memory_permission.hpp"

#include <bit>
#include <cstdint>
#include <cstring>
#include <limits>
#include <optional>
#include <span>

namespace ilemu {
namespace {
    constexpr std::uint32_t invalid_argument = 22U;
    constexpr std::uint32_t bad_address = 14U;
    constexpr std::uint32_t header_size = 40U;
    constexpr std::uint32_t maximum_slide_info_size = 2560U * 1024U;
    constexpr std::uint16_t no_rebase = 0x4000U;
    constexpr std::uint16_t extra_chain = 0x8000U;
    constexpr std::uint16_t chain_value = 0x3fffU;

    struct SlideInfo {
        std::uint32_t page_starts_offset;
        std::uint32_t page_starts_count;
        std::uint32_t page_extras_offset;
        std::uint32_t page_extras_count;
        std::uint32_t delta_mask;
        std::uint32_t value_add;
    };

    [[nodiscard]] bool within(
        std::uint32_t offset, std::uint64_t length, std::uint32_t size)
    {
        return offset <= size && length <= size - offset;
    }

    [[nodiscard]] std::optional<SlideInfo> read_info(const AddressSpace& memory,
        std::uint32_t address, std::uint32_t size, std::uint32_t mapping_size)
    {
        if (address == 0U || size < header_size ||
            size > maximum_slide_info_size ||
            size > std::numeric_limits<std::uint32_t>::max() - address ||
            !memory.accessible(address, size, MemoryPermission::Read))
            return std::nullopt;
        const auto version = memory.read32(address);
        const auto page_size = memory.read32(address + 4U);
        const auto starts_offset = memory.read32(address + 8U);
        const auto starts_count = memory.read32(address + 12U);
        const auto extras_offset = memory.read32(address + 16U);
        const auto extras_count = memory.read32(address + 20U);
        const auto delta_mask = memory.read64(address + 24U);
        const auto value_add = memory.read64(address + 32U);
        if (!version || !page_size || !starts_offset || !starts_count ||
            !extras_offset || !extras_count || !delta_mask || !value_add ||
            *version != 2U || *page_size != AddressSpace::page_size ||
            *starts_count > mapping_size / AddressSpace::page_size ||
            !within(*starts_offset,
                static_cast<std::uint64_t>(*starts_count) * 2U, size) ||
            !within(*extras_offset,
                static_cast<std::uint64_t>(*extras_count) * 2U, size) ||
            *delta_mask == 0U || *delta_mask > UINT32_MAX ||
            std::countr_zero(static_cast<std::uint32_t>(*delta_mask)) < 2U ||
            *value_add > UINT32_MAX)
            return std::nullopt;
        return SlideInfo { *starts_offset, *starts_count, *extras_offset,
            *extras_count, static_cast<std::uint32_t>(*delta_mask),
            static_cast<std::uint32_t>(*value_add) };
    }

    [[nodiscard]] std::uint32_t decode_chain(std::span<std::byte> page,
        std::uint32_t start, const SlideInfo& info, std::uint32_t slide)
    {
        constexpr auto last_pointer = AddressSpace::page_size - 4U;
        if (start > last_pointer)
            return invalid_argument;
        const auto little_endian = [](std::uint32_t value) {
            if constexpr (std::endian::native == std::endian::little)
                return value;
            else
                return __builtin_bswap32(value);
        };
        const auto shift = std::countr_zero(info.delta_mask) - 2U;
        auto offset = start;
        while (offset <= last_pointer) {
            std::uint32_t encoded;
            std::memcpy(&encoded, page.data() + offset, sizeof(encoded));
            encoded = little_endian(encoded);
            const auto delta = (encoded & info.delta_mask) >> shift;
            auto pointer = encoded & ~info.delta_mask;
            if (pointer != 0U)
                pointer += info.value_add + slide;
            pointer = little_endian(pointer);
            std::memcpy(page.data() + offset, &pointer, sizeof(pointer));
            if (delta == 0U)
                return 0U;
            offset += delta;
        }
        return invalid_argument;
    }

    [[nodiscard]] std::uint32_t decode_page(const AddressSpace& memory,
        std::span<std::byte> page, std::uint16_t entry, const SlideInfo& info,
        std::uint32_t slide, std::uint32_t slide_info_address)
    {
        if ((entry & extra_chain) == 0U)
            return decode_chain(
                page, static_cast<std::uint32_t>(entry) << 2U, info, slide);
        auto index = static_cast<std::uint32_t>(entry & chain_value);
        while (index < info.page_extras_count) {
            const auto extra = memory.read16(
                slide_info_address + info.page_extras_offset + index * 2U);
            if (!extra)
                return bad_address;
            const auto error = decode_chain(page,
                static_cast<std::uint32_t>(*extra & chain_value) << 2U, info,
                slide);
            if (error != 0U)
                return error;
            if ((*extra & extra_chain) != 0U)
                return 0U;
            ++index;
        }
        return invalid_argument;
    }
} // namespace

std::uint32_t apply_shared_region_slide_v2(AddressSpace& memory,
    std::uint32_t mapping_address, std::uint32_t mapping_size,
    std::uint32_t slide, std::uint32_t slide_info_address,
    std::uint32_t slide_info_size)
{
    if (mapping_size == 0U ||
        mapping_size >
            std::numeric_limits<std::uint32_t>::max() - mapping_address)
        return invalid_argument;
    const auto info =
        read_info(memory, slide_info_address, slide_info_size, mapping_size);
    if (!info)
        return invalid_argument;

    for (std::uint32_t page = 0U; page < info->page_starts_count; ++page) {
        const auto entry = memory.read16(
            slide_info_address + info->page_starts_offset + page * 2U);
        if (!entry)
            return bad_address;
        if (*entry == no_rebase)
            continue;
        const auto page_address =
            mapping_address + page * AddressSpace::page_size;
        auto bytes = memory.read_bytes(page_address, AddressSpace::page_size);
        if (!bytes)
            return bad_address;
        // Extra chains observe earlier changes in this same page, as in XNU.
        // Copy, intern and publish once per page, including a modified prefix
        // on error. Loader copy-in preserves the mapping's guest permissions.
        const auto error = decode_page(
            memory, *bytes, *entry, *info, slide, slide_info_address);
        if (!memory.copy_in_immutable_page(page_address, *bytes))
            return bad_address;
        if (error != 0U)
            return error;
    }
    return 0U;
}
} // namespace ilemu
