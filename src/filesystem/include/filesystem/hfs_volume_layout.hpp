// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// Discover guest HFS mount identities and geometry from the firmware
// filesystem.

#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

#include "filesystem/hfs_metadata.hpp"
#include "filesystem/hfs_quota.hpp"

namespace ilemu::hfs {

// Guest-visible HFS volumes discovered from the firmware's own fstab. This
// keeps partition identity in the filesystem layer and avoids leaking host
// filesystem geometry through statfs/getattrlist.
class VolumeLayout {
public:
    VolumeLayout(std::filesystem::path rootfs, std::uint64_t storage_bytes);

    // The directory backend has no HFS allocation/catalog blocks. Expose its
    // volume header for native filesystem identification and quick checks;
    // requests for on-disk file records remain unsupported.
    [[nodiscard]] static std::optional<std::vector<std::byte>> read_device(
        const VolumeMetadata& volume, std::uint64_t offset, std::size_t size);
    [[nodiscard]] static std::uint64_t device_size(const VolumeMetadata& volume);
    [[nodiscard]] const VolumeMetadata& for_guest_path(
        std::string_view path) const;
    [[nodiscard]] const VolumeMetadata& for_host_path(
        const std::filesystem::path& path) const;
    [[nodiscard]] const VolumeMetadata& for_mounted_device(
        std::string_view device) const;
    [[nodiscard]] bool is_mount_root(std::string_view path) const;
    // Host namespace work keeps its own volume records while sharing the
    // quota ledger, whose updates are serialized internally.
    [[nodiscard]] VolumeLayout snapshot() const
    {
        auto result = *this;
        result.volumes_ = std::make_shared<std::vector<VolumeMetadata>>(*volumes_);
        return result;
    }
    [[nodiscard]] std::span<const VolumeMetadata> volumes() const
    {
        for (auto& volume : *volumes_)
            static_cast<void>(for_guest_path(volume.mount_point));
        return *volumes_;
    }
    Quotas quotas;

private:
    std::filesystem::path rootfs_;
    std::shared_ptr<std::vector<VolumeMetadata>> volumes_ {
        std::make_shared<std::vector<VolumeMetadata>>() };
};

} // namespace ilemu::hfs
