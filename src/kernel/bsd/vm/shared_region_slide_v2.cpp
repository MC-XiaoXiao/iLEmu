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
#include <cstring>
#include <cstdint>
#include <limits>
#include <optional>

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

    [[nodiscard]] bool within(std::uint32_t offset, std::uint64_t length,
        std::uint32_t size)
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

    class SlideWriteWindow {
    public:
        SlideWriteWindow(AddressSpace& memory, std::uint32_t address,
            std::uint32_t size, MemoryPermission original)
            : memory_ { memory }, address_ { address }, size_ { size }
            , original_ { original }
            , writable_ { memory.protect(address, size,
                  original | MemoryPermission::Write) }
        {
        }

        ~SlideWriteWindow()
        {
            if (writable_)
                static_cast<void>(memory_.protect(address_, size_, original_));
        }

        [[nodiscard]] bool writable() const { return writable_; }

    private:
        AddressSpace& memory_;
        std::uint32_t address_;
        std::uint32_t size_;
        MemoryPermission original_;
        bool writable_;
    };

    [[nodiscard]] std::uint32_t decode_chain(AddressSpace& memory,
        std::uint32_t page_address, std::uint32_t start,
        const SlideInfo& info, std::uint32_t slide)
    {
        constexpr auto last_pointer = AddressSpace::page_size - 4U;
        if (start > last_pointer)
            return invalid_argument;
        auto page = memory.read_bytes(page_address, AddressSpace::page_size);
        if (!page)
            return bad_address;
        const auto little_endian = [](std::uint32_t value) {
            if constexpr (std::endian::native == std::endian::little)
                return value;
            else
                return __builtin_bswap32(value);
        };
        const auto shift = std::countr_zero(info.delta_mask) - 2U;
        auto offset = start;
        auto error = invalid_argument;
        while (offset <= last_pointer) {
            std::uint32_t encoded;
            std::memcpy(&encoded, page->data() + offset, sizeof(encoded));
            encoded = little_endian(encoded);
            const auto delta = (encoded & info.delta_mask) >> shift;
            auto pointer = encoded & ~info.delta_mask;
            if (pointer != 0U)
                pointer += info.value_add + slide;
            pointer = little_endian(pointer);
            std::memcpy(page->data() + offset, &pointer, sizeof(pointer));
            if (delta == 0U) {
                error = 0U;
                break;
            }
            offset += delta;
        }
        // A chain never crosses its page. Commit its modified prefix even on
        // a malformed terminating delta, as the scalar decoder does. Flush
        // before reading the next extra chain so overlapping inputs retain
        // their ordering, while COW and VM accounting happen once per chain.
        return memory.copy_in_immutable_page(page_address, *page) ? error : bad_address;
    }
} // namespace

std::uint32_t apply_shared_region_slide_v2(AddressSpace& memory,
    std::uint32_t mapping_address, std::uint32_t mapping_size,
    std::uint32_t initial_protection, std::uint32_t slide,
    std::uint32_t slide_info_address, std::uint32_t slide_info_size)
{
    if (mapping_size == 0U ||
        mapping_size > std::numeric_limits<std::uint32_t>::max() - mapping_address)
        return invalid_argument;
    const auto info = read_info(
        memory, slide_info_address, slide_info_size, mapping_size);
    if (!info)
        return invalid_argument;
    const auto original = static_cast<MemoryPermission>(
        initial_protection & 7U);
    SlideWriteWindow writable { memory, mapping_address, mapping_size, original };
    if (!writable.writable())
        return invalid_argument;

    for (std::uint32_t page = 0U; page < info->page_starts_count; ++page) {
        const auto entry = memory.read16(slide_info_address +
            info->page_starts_offset + page * 2U);
        if (!entry)
            return bad_address;
        if (*entry == no_rebase)
            continue;
        const auto page_address = mapping_address +
                                  page * AddressSpace::page_size;
        if ((*entry & extra_chain) == 0U) {
            const auto error = decode_chain(
                memory, page_address, static_cast<std::uint32_t>(*entry) << 2U,
                *info, slide);
            if (error != 0U)
                return error;
            continue;
        }
        auto index = static_cast<std::uint32_t>(*entry & chain_value);
        bool complete = false;
        while (index < info->page_extras_count) {
            const auto extra = memory.read16(slide_info_address +
                info->page_extras_offset + index * 2U);
            if (!extra)
                return bad_address;
            const auto error = decode_chain(memory, page_address,
                static_cast<std::uint32_t>(*extra & chain_value) << 2U,
                *info, slide);
            if (error != 0U)
                return error;
            if ((*extra & extra_chain) != 0U) {
                complete = true;
                break;
            }
            ++index;
        }
        if (!complete)
            return invalid_argument;
    }
    return 0U;
}
} // namespace ilemu
