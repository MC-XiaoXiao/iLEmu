// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// XNU's vm_shared_region_slide_page_v2 wire format and pointer-chain rules:
// https://github.com/apple-oss-distributions/xnu/blob/xnu-3789.1.32/osfmk/vm/vm_shared_region.c

#include "shared_region_slide_v2.hpp"

#include "foundation/address_space.hpp"
#include "foundation/file_page_transform.hpp"
#include "foundation/memory_permission.hpp"

#include <bit>
#include <cstdint>
#include <cstring>
#include <limits>
#include <optional>
#include <span>
#include <utility>
#include <vector>

namespace ilemu {
namespace {
    constexpr std::uint32_t invalid_argument = 22U;
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

    [[nodiscard]] std::uint16_t table_entry(
        std::span<const std::byte> metadata, std::uint32_t offset)
    {
        return static_cast<std::uint16_t>(
            std::to_integer<std::uint16_t>(metadata[offset]) |
            (std::to_integer<std::uint16_t>(metadata[offset + 1U]) << 8U));
    }

    [[nodiscard]] std::uint32_t decode_page(std::span<const std::byte> metadata,
        std::span<std::byte> page, std::uint16_t entry, const SlideInfo& info,
        std::uint32_t slide)
    {
        if ((entry & extra_chain) == 0U)
            return decode_chain(
                page, static_cast<std::uint32_t>(entry) << 2U, info, slide);
        auto index = static_cast<std::uint32_t>(entry & chain_value);
        while (index < info.page_extras_count) {
            const auto extra =
                table_entry(metadata, info.page_extras_offset + index * 2U);
            const auto error = decode_chain(page,
                static_cast<std::uint32_t>(extra & chain_value) << 2U, info,
                slide);
            if (error != 0U)
                return error;
            if ((extra & extra_chain) != 0U)
                return 0U;
            ++index;
        }
        return invalid_argument;
    }

    class SharedRegionSlideV2 final : public FilePageTransform {
    public:
        SharedRegionSlideV2(std::vector<std::byte> metadata, SlideInfo info,
            std::uint64_t file_offset, std::uint32_t slide)
            : metadata_ { std::move(metadata) }
            , info_ { info }
            , file_offset_ { file_offset }
            , slide_ { slide }
        {
        }

        std::shared_ptr<GuestPageBacking> apply(FilePageCache& cache,
            std::shared_ptr<GuestPageBacking> source,
            std::uint64_t file_offset) const override
        {
            const auto page =
                (file_offset - file_offset_) / AddressSpace::page_size;
            // The mapping can extend beyond the firmware's rebase table.
            if (page >= info_.page_starts_count)
                return source;
            const auto entry = table_entry(
                metadata_, info_.page_starts_offset +
                               static_cast<std::uint32_t>(page) * 2U);
            if (entry == no_rebase)
                return source;
            auto bytes = source->bytes;
            if (decode_page(metadata_, bytes, entry, info_, slide_) != 0U)
                return { };
            return cache.intern_immutable_page(bytes);
        }

    private:
        std::vector<std::byte> metadata_;
        SlideInfo info_;
        std::uint64_t file_offset_;
        std::uint32_t slide_;
    };
} // namespace

std::shared_ptr<const FilePageTransform> prepare_shared_region_slide_v2(
    const AddressSpace& memory, std::uint32_t mapping_size,
    std::uint64_t file_offset, std::uint32_t slide,
    std::uint32_t slide_info_address, std::uint32_t slide_info_size)
{
    if (mapping_size == 0U)
        return { };
    const auto info =
        read_info(memory, slide_info_address, slide_info_size, mapping_size);
    if (!info)
        return { };
    auto metadata = memory.read_bytes(slide_info_address, slide_info_size);
    if (!metadata)
        return { };
    return std::make_shared<SharedRegionSlideV2>(
        std::move(*metadata), *info, file_offset, slide);
}
} // namespace ilemu
