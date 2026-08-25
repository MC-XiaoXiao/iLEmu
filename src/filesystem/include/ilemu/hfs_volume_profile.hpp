#pragma once

#include <cstdint>
#include <filesystem>
#include <span>
#include <string_view>
#include <vector>

#include "ilemu/hfs_metadata.hpp"

namespace ilemu::hfs {

// Guest-visible HFS volumes discovered from the firmware's own fstab. This
// keeps partition identity in the filesystem layer and avoids leaking host
// filesystem geometry through statfs/getattrlist.
class VolumeProfile {
public:
    VolumeProfile(std::filesystem::path rootfs, std::uint64_t storage_bytes);

    [[nodiscard]] const VolumeMetadata& for_guest_path(
        std::string_view path) const;
    [[nodiscard]] const VolumeMetadata& for_mounted_device(
        std::string_view device) const;
    [[nodiscard]] bool is_mount_root(std::string_view path) const;
    [[nodiscard]] std::span<const VolumeMetadata> volumes() const
    {
        return volumes_;
    }

private:
    std::vector<VolumeMetadata> volumes_;
};

} // namespace ilemu::hfs
