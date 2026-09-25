// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "filesystem/hfs_metadata.hpp"
#include <algorithm>
#include <array>

namespace ilemu::hfs {
namespace {
    // ARM32 Darwin attrlist layout. RETURNED_ATTRS and ERROR precede NAME.
    // Recognized optional attributes without backend data remain invalid; their
    // slots are present only with FSOPT_PACK_INVAL_ATTRS.
    constexpr std::uint32_t error_attribute = 0x20000000U;
    constexpr std::array<std::size_t, 32> common_sizes { 8, 4, 8, 4, 4, 8, 8, 8,
        4, 8, 8, 8, 8, 8, 32, 4, 4, 4, 4, 4, 4, 4, 8, 16, 16, 8, 8, 8, 8, 4, 4,
        20 };
    struct Field {
        std::size_t group;
        std::uint32_t bit;
        std::size_t size;
    };
    using Masks = std::array<std::uint32_t, 5>;

    std::uint32_t read_word(
        const std::vector<std::byte>& bytes, std::size_t offset)
    {
        std::uint32_t value = 0;
        for (unsigned i = 0; i != 4; ++i)
            value |= std::to_integer<std::uint32_t>(bytes[offset + i])
                     << (8 * i);
        return value;
    }
    void write_word(
        std::vector<std::byte>& bytes, std::size_t offset, std::uint32_t value)
    {
        for (unsigned i = 0; i != 4; ++i)
            bytes[offset + i] = static_cast<std::byte>(value >> (8 * i));
    }
    std::vector<Field> fields(const Masks& masks, bool directory)
    {
        std::vector<Field> result;
        const auto add = [&](std::size_t group, std::uint32_t bit,
                             std::size_t size) {
            if (masks[group] & bit)
                result.push_back({ group, bit, size });
        };
        add(0, error_attribute, 4);
        for (unsigned i = 0; i != 31; ++i) {
            if (i != 29)
                add(0, 1U << i, common_sizes[i]);
        }
        if (directory) {
            for (unsigned i = 0; i != 3; ++i)
                add(2, 1U << i, 4);
        } else {
            for (unsigned i = 0; i != 14; ++i) {
                const auto bit = 1U << i;
                if (attribute::file_supported_mask & bit)
                    add(3, bit, (bit & 0x29U) ? 4 : 8);
            }
        }
        return result;
    }
} // namespace

bool MetadataProvider::valid_bulk_request(const AttributeRequest& request)
{
    constexpr auto required =
        attribute::common_name | attribute::common_returned_attributes;
    return request.volume == 0 && request.fork == 0 &&
           (request.common & required) == required &&
           !(request.directory & ~attribute::directory_supported_mask) &&
           !(request.file & ~attribute::file_supported_mask);
}

std::vector<std::byte> MetadataProvider::pack_bulk_attributes(
    const Metadata& metadata, const AttributeRequest& request,
    std::string_view guest_path, bool pack_invalid, std::uint32_t error)
{
    using namespace attribute;
    AttributeRequest base {
        .common = request.common & common_supported_mask,
        .directory = metadata.directory ? request.directory : 0,
        .file = metadata.directory ? 0 : request.file,
    };
    if (error) {
        base.common = common_name | common_returned_attributes;
        base.directory = base.file = 0;
    }
    const auto source = pack_attributes(metadata, base, guest_path);
    const Masks source_masks { base.common, 0, base.directory, base.file, 0 };
    auto actual = source_masks;
    actual[0] |= request.common & error_attribute;
    const Masks layout = pack_invalid ? Masks { request.common, 0,
        metadata.directory ? request.directory : 0,
        metadata.directory ? 0 : request.file, 0 }
                                      : actual;
    const auto layout_fields = fields(layout, metadata.directory);
    std::size_t fixed_size = 24;
    for (const auto& field : layout_fields)
        fixed_size += field.size;
    std::vector<std::byte> result(fixed_size);
    for (std::size_t i = 0; i != actual.size(); ++i)
        write_word(result, 4 + 4 * i, actual[i]);
    std::size_t cursor = 24;
    std::size_t source_cursor = 24;
    for (const auto& field : layout_fields) {
        if (field.group == 0 && field.bit == error_attribute) {
            write_word(result, cursor, error);
        } else if (source_masks[field.group] & field.bit) {
            if (field.group == 0 &&
                (field.bit == common_name || field.bit == common_full_path)) {
                const auto offset =
                    source_cursor + read_word(source, source_cursor);
                const auto length = read_word(source, source_cursor + 4);
                const auto target = result.size();
                result.resize(target + ((length + 3U) & ~3U));
                std::copy_n(
                    source.begin() + offset, length, result.begin() + target);
                write_word(result, cursor,
                    static_cast<std::uint32_t>(target - cursor));
                write_word(result, cursor + 4, length);
            } else {
                std::copy_n(source.begin() + source_cursor, field.size,
                    result.begin() + cursor);
            }
            source_cursor += field.size;
        }
        cursor += field.size;
    }
    write_word(result, 0, static_cast<std::uint32_t>(result.size()));
    return result;
}
} // namespace ilemu::hfs
