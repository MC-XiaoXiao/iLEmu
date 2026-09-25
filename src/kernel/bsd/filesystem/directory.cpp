// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "../support.hpp"
#include "kernel/baseband_device.hpp"
#include "kernel/darwin_abi.hpp"
#include "kernel/kernel.hpp"
#include "kernel/offline_serial_device.hpp"
#include <algorithm>
#include <array>
#include <limits>

namespace ilemu {
const std::vector<CompatibilityKernel::DirectoryEntry>*
CompatibilityKernel::cached_directory_entries(
    const std::filesystem::path& path, std::uint32_t& error)
{
    std::error_code directory_error;
    const auto cache_key = path.lexically_normal();
    auto cached_entries = directory_entries_cache_.find(cache_key);
    if (cached_entries == directory_entries_cache_.end()) {
        const auto current_metadata =
            hfs_metadata_.query_directory_entry(path, true);
        const auto parent_path = path == rootfs_ ? path : path.parent_path();
        const auto parent_metadata =
            hfs_metadata_.query_directory_entry(parent_path, true);
        std::vector<DirectoryEntry> entries {
            { ".", 4, current_metadata ? current_metadata->catalog_id : 2U },
            { "..", 4, parent_metadata ? parent_metadata->catalog_id : 2U }
        };
        for (std::filesystem::directory_iterator
                 iterator { path, directory_error },
            end;
            !directory_error && iterator != end;
            iterator.increment(directory_error)) {
            if (hfs::MetadataProvider::is_resource_sidecar(iterator->path())) {
                continue;
            }
            const auto metadata =
                hfs_metadata_.query_directory_entry(iterator->path(), false);
            if (!metadata) {
                directory_error = std::make_error_code(std::errc::io_error);
                break;
            }
            std::uint8_t type = 0;
            if (metadata->type == std::filesystem::file_type::directory)
                type = 4;
            else if (metadata->type == std::filesystem::file_type::regular)
                type = 8;
            else if (metadata->type == std::filesystem::file_type::symlink)
                type = 10;
            entries.push_back({ iterator->path().filename().string(), type,
                metadata->catalog_id });
        }
        if (directory_error) {
            error = 5;
            return nullptr;
        }
        std::error_code dev_directory_error;
        if (std::filesystem::equivalent(
                path, rootfs_ / "dev", dev_directory_error) &&
            !dev_directory_error) {
            const auto add_virtual = [&](std::string name, std::uint8_t type) {
                if (std::none_of(entries.begin(), entries.end(),
                        [&](const DirectoryEntry& entry) {
                            return entry.name == name;
                        })) {
                    constexpr std::uint32_t first_virtual_catalog_id =
                        0x7fff0000U;
                    entries.push_back({ std::move(name), type,
                        first_virtual_catalog_id +
                            static_cast<std::uint32_t>(entries.size()) });
                }
            };
            add_virtual("disk0s1", 6); // DT_BLK
            add_virtual("disk0s2", 6);
            add_virtual("rdisk0s1", 2); // DT_CHR
            add_virtual("rdisk0s2", 2);
            add_virtual("console", 2);
            add_virtual("null", 2);
            add_virtual("autofs_nowait", 2);
            add_virtual("random", 2);
            add_virtual("urandom", 2);
            add_virtual("bpf0", 2);
            if (shared_state_->baseband_device_state.available()) {
                add_virtual(
                    std::string { bsd::baseband_device::legacy_path.substr(5) },
                    2);
                add_virtual(
                    std::string { bsd::baseband_device::directory_name }, 2);
                add_virtual(
                    std::string {
                        bsd::baseband_device::spi_mux_directory_name },
                    2);
                add_virtual(
                    std::string { bsd::baseband_device::h5_mux_directory_name },
                    2);
            }
            add_virtual(
                std::string { bsd::offline_serial_device::directory_name }, 2);
        }
        std::sort(entries.begin() + 2, entries.end(),
            [](const DirectoryEntry& lhs, const DirectoryEntry& rhs) {
                return lhs.name < rhs.name;
            });
        cached_entries =
            directory_entries_cache_.emplace(cache_key, std::move(entries))
                .first;
    }
    return &cached_entries->second;
}

// Darwin getattrlistbulk(2), using the same directory position and catalog
// entries as getdirentries. Each complete record commits its own offset.
void CompatibilityKernel::dispatch_bsd_directory_attributes(Cpu& cpu)
{
    const auto& registers = cpu.registers();
    auto fd = registers[0];
    if (const auto duplicate = duplicated_descriptors_.find(fd);
        duplicate != duplicated_descriptors_.end())
        fd = duplicate->second;
    const auto descriptor = file_descriptors_.find(fd);
    const auto flags =
        file_status_flags_.contains(fd) ? file_status_flags_.at(fd) : 0U;
    if (descriptor == file_descriptors_.end() ||
        (flags & darwin::open_flag::access_mode) ==
            darwin::open_flag::write_only) {
        bsd_error(cpu, bsd_support::bad_file_descriptor);
        return;
    }
    std::error_code host_error;
    if (!std::filesystem::is_directory(descriptor->second, host_error)) {
        bsd_error(cpu, 20); // ENOTDIR
        return;
    }
    const auto attributes = memory_.read_bytes(registers[1], 24);
    if (!attributes) {
        bsd_error(cpu, bsd_support::bad_address);
        return;
    }
    const auto read_word = [&](std::size_t offset) {
        std::uint32_t value = 0;
        for (unsigned i = 0; i != 4; ++i)
            value |= std::to_integer<std::uint32_t>((*attributes)[offset + i])
                     << (8 * i);
        return value;
    };
    const hfs::AttributeRequest request { read_word(4), read_word(8),
        read_word(12), read_word(16), read_word(20) };
    if ((read_word(0) & 0xffffU) != 5 ||
        !hfs::MetadataProvider::valid_bulk_request(request)) {
        bsd_error(cpu, bsd_support::invalid_argument);
        return;
    }
    // XNU cannot even inspect an entry unless length, returned masks and
    // NAME's attrreference fit, plus at least one byte of variable data.
    if (registers[3] <= 32U) {
        bsd_error(cpu, 34U); // ERANGE, including zero-sized requests at EOF
        return;
    }
    std::uint32_t error = 0;
    const auto* entries = cached_directory_entries(descriptor->second, error);
    if (!entries) {
        bsd_error(cpu, error);
        return;
    }
    auto index = std::min<std::uint64_t>(file_offsets_[fd], entries->size());
    std::uint32_t count = 0;
    std::uint64_t copied = 0;
    const auto buffer_size = registers[3];
    const auto buffer = registers[2];
    const bool pack_invalid = (registers[4] & 8U) != 0;
    while (index < entries->size()) {
        const auto& entry = (*entries)[index];
        if (entry.name == "." || entry.name == "..") {
            file_offsets_[fd] = ++index;
            continue;
        }
        const auto path = descriptor->second / entry.name;
        const auto metadata = query_hfs_metadata(path, false,
            (request.directory & hfs::attribute::directory_entry_count) != 0);
        if (!metadata) {
            // A cached entry may have disappeared since the last read.
            file_offsets_[fd] = ++index;
            continue;
        }
        const auto guest_path =
            (std::filesystem::path { "/" } / path.lexically_relative(rootfs_))
                .lexically_normal()
                .generic_string();
        auto record = hfs::MetadataProvider::pack_bulk_attributes(
            *metadata, request, guest_path, pack_invalid);
        const auto remaining = buffer_size - copied;
        if (record.size() > remaining)
            break;
        const auto aligned = (record.size() + 7U) & ~std::size_t { 7 };
        if (aligned <= remaining)
            record.resize(aligned);
        for (unsigned i = 0; i != 4; ++i)
            record[i] = static_cast<std::byte>(record.size() >> (8 * i));
        if (static_cast<std::uint64_t>(buffer) + copied + record.size() >
                (std::uint64_t { 1 } << 32U) ||
            !memory_.copy_in(
                static_cast<std::uint32_t>(buffer + copied), record)) {
            error = bsd_support::bad_address;
            break;
        }
        copied += record.size();
        ++count;
        file_offsets_[fd] = ++index;
    }
    if (count != 0 || (!error && index == entries->size())) {
        bsd_success(cpu, count);
    } else {
        bsd_error(cpu, error ? error : 34U); // ERANGE unless true EOF
    }
}
} // namespace ilemu
