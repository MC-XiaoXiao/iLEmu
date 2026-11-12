// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// Discover guest HFS mount identities and geometry from the firmware
// filesystem.

#include "filesystem/hfs_volume_layout.hpp"

#include <algorithm>
#include <openssl/evp.h>
#include <fstream>
#include <limits>
#include <sstream>
#include <string>
#include <system_error>

namespace ilemu::hfs {
namespace {

    constexpr std::uint32_t mount_read_only = 0x00000001U;
    constexpr std::uint32_t mount_no_execute = 0x00000004U;
    constexpr std::uint32_t mount_no_suid = 0x00000008U;
    constexpr std::uint32_t mount_no_devices = 0x00000010U;
    constexpr std::uint32_t mount_local = 0x00001000U;
    constexpr std::uint32_t mount_rootfs = 0x00004000U;

    std::array<std::byte, 16> volume_uuid(std::span<const std::byte> identifier)
    {
        std::array<std::byte, 16> result { };
        // XNU hfs_getvoluuid: UUIDv3 in the HFS namespace, using the eight
        // FinderInfo identifier bytes without endian-swapping them.
        std::vector<unsigned char> name {
            0xb3, 0xe2, 0x0f, 0x39, 0xf2, 0x92, 0x11, 0xd6,
            0x97, 0xa4, 0x00, 0x30, 0x65, 0x43, 0xec, 0xac
        };
        for (const auto byte : identifier)
            name.push_back(std::to_integer<unsigned char>(byte));
        unsigned int size = 0;
        if (EVP_Digest(name.data(), name.size(),
                reinterpret_cast<unsigned char*>(result.data()), &size,
                EVP_md5(), nullptr) != 1 || size != result.size())
            return { };
        result[6] = (result[6] & std::byte { 0x0f }) | std::byte { 0x30 };
        result[8] = (result[8] & std::byte { 0x3f }) | std::byte { 0x80 };
        return result;
    }

    struct DataUsage {
        std::uint64_t bytes { };
        std::uint32_t files { };
        std::uint32_t directories { };
    };

    DataUsage data_usage(const std::filesystem::path& path)
    {
        DataUsage usage;
        std::error_code error;
        if (!std::filesystem::exists(path, error))
            return usage;
        for (std::filesystem::recursive_directory_iterator iterator { path,
                 std::filesystem::directory_options::skip_permission_denied,
                 error },
            end;
            !error && iterator != end; iterator.increment(error)) {
            const auto status = iterator->symlink_status(error);
            if (error)
                break;
            if (std::filesystem::is_directory(status)) {
                ++usage.directories;
            } else if (std::filesystem::is_regular_file(status)) {
                usage.bytes += iterator->file_size(error);
                if (error)
                    break;
                ++usage.files;
            }
        }
        return usage;
    }

    std::uint32_t mount_flags(
        std::string_view mount_point, std::string_view options)
    {
        auto result = mount_local;
        if (mount_point == "/")
            result |= mount_rootfs;
        std::size_t cursor = 0;
        while (cursor <= options.size()) {
            const auto separator = options.find(',', cursor);
            const auto option = options.substr(cursor,
                separator == std::string_view::npos ? options.size() - cursor
                                                    : separator - cursor);
            if (option == "ro")
                result |= mount_read_only;
            else if (option == "noexec")
                result |= mount_no_execute;
            else if (option == "nosuid")
                result |= mount_no_suid;
            else if (option == "nodev")
                result |= mount_no_devices;
            if (separator == std::string_view::npos)
                break;
            cursor = separator + 1;
        }
        return result;
    }

    bool contains_path(std::string_view mount_point, std::string_view path)
    {
        if (mount_point == "/")
            return !path.empty() && path.front() == '/';
        return path == mount_point || (path.size() > mount_point.size() &&
                                          path.starts_with(mount_point) &&
                                          path[mount_point.size()] == '/');
    }

    std::string normalize_guest_path(std::string_view path)
    {
        if (path == "/var")
            return "/private/var";
        if (path.starts_with("/var/"))
            return "/private" + std::string { path };
        if (path.empty() || path.front() != '/')
            return "/" + std::string { path };
        return std::string { path };
    }

    VolumeMetadata data_volume(std::string source, std::string mount_point,
        std::string_view options, const std::filesystem::path& rootfs,
        std::uint64_t storage_bytes, std::uint32_t system_blocks)
    {
        VolumeMetadata volume;
        volume.name = "Data";
        volume.mount_point = std::move(mount_point);
        volume.mounted_device = std::move(source);
        volume.mount_flags = mount_flags(volume.mount_point, options);
        const auto storage_blocks =
            std::min<std::uint64_t>(storage_bytes / volume.block_size,
                std::numeric_limits<std::uint32_t>::max());
        volume.total_blocks = static_cast<std::uint32_t>(
            storage_blocks > system_blocks ? storage_blocks - system_blocks
                                           : 1U);
        const auto relative =
            std::filesystem::path { volume.mount_point }.relative_path();
        const auto usage = data_usage(rootfs / relative);
        const auto used_blocks = std::min<std::uint64_t>(
            (usage.bytes + volume.block_size - 1U) / volume.block_size,
            volume.total_blocks);
        volume.free_blocks =
            volume.total_blocks - static_cast<std::uint32_t>(used_blocks);
        volume.file_count = usage.files;
        volume.directory_count = usage.directories + 1U;
        volume.next_catalog_id =
            16U + volume.file_count + volume.directory_count;
        return volume;
    }

} // namespace

VolumeLayout::VolumeLayout(
    std::filesystem::path rootfs, std::uint64_t storage_bytes)
    : rootfs_(rootfs)
{
    volumes_->push_back(VolumeMetadata { });
    // Retain just the extracted volume header, never the disk image, when
    // original firmware geometry is available.
    std::array<std::byte, 512> header { };
    std::ifstream metadata { rootfs.parent_path() / "firmware/system.hfsx.header",
        std::ios::binary };
    const auto has_header = static_cast<bool>(metadata.read(
        reinterpret_cast<char*>(header.data()), header.size()));
    if (has_header) {
        const auto word = [&](std::size_t at, unsigned size = 4U) {
            std::uint32_t value = 0;
            for (unsigned i = 0; i < size; ++i)
                value = (value << 8U) | std::to_integer<unsigned>(header[at + i]);
            return value;
        };
        auto& system = volumes_->front();
        system.signature = word(0, 2);
        system.version = word(2, 2);
        system.attributes = word(4);
        system.last_mounted_version = word(8);
        system.file_count = word(32);
        system.directory_count = word(36);
        system.block_size = word(40);
        system.total_blocks = word(44);
        system.free_blocks = word(48);
        system.allocation_clump = word(60);
        system.next_catalog_id = word(64);
        system.encodings_used = (static_cast<std::uint64_t>(word(72)) << 32U) | word(76);
        std::copy_n(header.begin() + 104, 8, system.finder_identifier.begin());
    }
    const auto fstab = rootfs / "private/etc/fstab";
    std::ifstream input { fstab };
    if (!input)
        input.open(rootfs / "etc/fstab");

    std::string line;
    while (std::getline(input, line)) {
        const auto comment = line.find('#');
        if (comment != std::string::npos)
            line.erase(comment);
        std::istringstream parser { line };
        std::string source;
        std::string mount_point;
        std::string type;
        std::string options;
        if (!(parser >> source >> mount_point >> type >> options) ||
            type != "hfs")
            continue;
        if (mount_point == "/") {
            auto& system = volumes_->front();
            system.mount_point = mount_point;
            system.mounted_device = source;
            system.mount_flags = mount_flags(mount_point, options);
            continue;
        }
        if (std::ranges::any_of(*volumes_, [&](const auto& volume) {
                return volume.mount_point == mount_point;
            })) {
            continue;
        }
        volumes_->push_back(
            data_volume(std::move(source), std::move(mount_point), options,
                rootfs, storage_bytes, volumes_->front().total_blocks));
    }
    for (auto& volume : *volumes_) {
        if (volume.mount_point != "/" || !has_header) {
            const auto identity = rootfs.generic_string() + volume.mount_point;
            const auto hash = volume_uuid(std::as_bytes(std::span { identity }));
            std::copy_n(hash.begin(), 8, volume.finder_identifier.begin());
        }
        volume.uuid = volume_uuid(volume.finder_identifier);
    }
}

std::uint64_t VolumeLayout::device_size(const VolumeMetadata& volume)
{
    return static_cast<std::uint64_t>(volume.total_blocks) * volume.block_size;
}

std::optional<std::vector<std::byte>> VolumeLayout::read_device(
    const VolumeMetadata& volume, std::uint64_t offset, std::size_t size)
{
    const auto length = device_size(volume);
    if (size == 0 || offset >= length)
        return std::vector<std::byte> { };
    size = static_cast<std::size_t>(std::min<std::uint64_t>(size, length - offset));
    // HFS reserves the first two and last sectors. Primary/alternate headers
    // occupy the third and penultimate sectors respectively (TN1150).
    // Native readers fetch headers through aligned read-ahead buffers. Their
    // envelope is zero-padded; a request wholly in the catalog/data area is
    // unsupported, rather than pretending the host tree is an HFS B-tree.
    if (offset >= 1536U && offset + size <= length - 1024U)
        return std::nullopt;
    std::array<std::byte, 512> header { };
    const auto word = [&](std::size_t at, std::uint32_t value, unsigned bytes = 4U) {
        for (unsigned i = 0; i < bytes; ++i)
            header[at + i] = static_cast<std::byte>(value >> ((bytes - i - 1U) * 8U));
    };
    word(0, volume.signature, 2);
    word(2, volume.version, 2);
    // File data and metadata are committed through the host filesystem, not
    // an HFS journal. There is no unreplayed HFS transaction in this backend.
    // Preserve the source's inconsistent/unclean state. A dirty extraction
    // cannot be declared clean by discarding its unavailable HFS journal.
    word(4, volume.attributes & ~(1U << 13U)); // no HFS journal in this backend
    word(8, volume.last_mounted_version);
    word(32, volume.file_count);
    word(36, volume.directory_count);
    word(40, volume.block_size);
    word(44, volume.total_blocks);
    word(48, volume.free_blocks);
    word(56, volume.allocation_clump);
    word(60, volume.allocation_clump);
    word(64, volume.next_catalog_id);
    word(72, static_cast<std::uint32_t>(volume.encodings_used >> 32U));
    word(76, static_cast<std::uint32_t>(volume.encodings_used));
    std::copy(volume.finder_identifier.begin(), volume.finder_identifier.end(),
        header.begin() + 104);
    std::vector<std::byte> result(size);
    for (const auto header_offset : { std::uint64_t { 1024 }, length - 1024U }) {
        const auto begin = std::max<std::uint64_t>(offset, header_offset);
        const auto end = std::min<std::uint64_t>(offset + size, header_offset + header.size());
        if (begin < end)
            std::copy_n(header.begin() + static_cast<std::ptrdiff_t>(begin - header_offset),
                end - begin, result.begin() + static_cast<std::ptrdiff_t>(begin - offset));
    }
    return result;
}

const VolumeMetadata& VolumeLayout::for_host_path(
    const std::filesystem::path& path) const
{
    return for_guest_path(path.lexically_relative(rootfs_).generic_string());
}

const VolumeMetadata& VolumeLayout::for_guest_path(std::string_view path) const
{
    const auto normalized = normalize_guest_path(path);
    VolumeMetadata* result = nullptr;
    for (auto& volume : *volumes_) {
        if (contains_path(volume.mount_point, normalized) &&
            (!result ||
                volume.mount_point.size() > result->mount_point.size())) {
            result = &volume;
        }
    }
    auto& volume = result ? *result : volumes_->front();
    constexpr std::uint32_t mount_quota = 0x00002000U;
    volume.mount_flags = (volume.mount_flags & ~mount_quota) |
        (quotas.active(volume.mount_point) ? mount_quota : 0U);
    return volume;
}

const VolumeMetadata& VolumeLayout::for_mounted_device(
    std::string_view device) const
{
    const auto found = std::ranges::find_if(*volumes_,
        [&](const auto& volume) { return volume.mounted_device == device; });
    return for_guest_path(found == volumes_->end() ? "/" : found->mount_point);
}

bool VolumeLayout::is_mount_root(std::string_view path) const
{
    const auto normalized =
        std::filesystem::path { normalize_guest_path(path) }.lexically_normal();
    return normalized.generic_string() == for_guest_path(path).mount_point;
}

} // namespace ilemu::hfs
