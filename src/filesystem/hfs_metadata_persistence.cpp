// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
#include "filesystem/hfs_metadata.hpp"

#include <cerrno>
#include <ctime>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/xattr.h>

namespace ilemu::hfs {
namespace {
    bool write_attribute(const Metadata& metadata, std::string_view name,
        const void* bytes, std::size_t size)
    {
        const auto key = std::string { name };
        if (metadata.host_descriptor >= 0)
            return ::fsetxattr(metadata.host_descriptor, key.c_str(), bytes,
                       size, 0) == 0;
        return ::lsetxattr(metadata.host_path.c_str(), key.c_str(), bytes, size,
                   0) == 0;
    }
    bool number(const Metadata& metadata, const char* name,
        const std::optional<std::uint32_t>& value)
    {
        if (!value)
            return true;
        const auto text = std::to_string(*value);
        return write_attribute(metadata,
            std::string { "user.hfsfuse.record." } + name, text.c_str(),
            text.size() + 1);
    }
    bool timestamp(const Metadata& metadata, const char* name,
        const std::optional<Timestamp>& value)
    {
        if (!value)
            return true;
        const time_t seconds = value->seconds;
        tm calendar { };
        char text[32];
        if (!::gmtime_r(&seconds, &calendar))
            return false;
        const auto size =
            ::strftime(text, sizeof(text), "%Y-%m-%dT%H:%M:%S+0000", &calendar);
        return size != 0 && write_attribute(metadata,
                                std::string { "user.hfsfuse.record." } + name,
                                text, size + 1);
    }
}

bool MetadataProvider::persist_override(
    const Metadata& metadata, MetadataOverride& update)
{
    if (metadata.object_type == 5U)
        return false; // Linux/Android do not permit user xattrs on symlinks.
    if (!number(metadata, "file_mode", update.mode) ||
        !number(metadata, "owner_id", update.owner) ||
        !number(metadata, "group_id", update.group) ||
        !number(metadata, "bsd_flags", update.flags) ||
        !number(metadata, "protection_class", update.data_protection_class) ||
        !timestamp(metadata, "date_created", update.creation_time) ||
        !timestamp(metadata, "date_backedup", update.backup_time))
        return false;
    if (update.finder_info &&
        !write_attribute(metadata, "user.com.apple.FinderInfo",
            update.finder_info->data(), update.finder_info->size()))
        return false;
    if (update.modification_time || update.access_time) {
        const auto host_time = [](const std::optional<Timestamp>& time) {
            return time ? timespec { time->seconds, time->nanoseconds }
                        : timespec { 0, UTIME_OMIT };
        };
        const timespec times[] = { host_time(update.access_time),
            host_time(update.modification_time) };
        const auto result =
            metadata.host_descriptor >= 0
                ? ::futimens(metadata.host_descriptor, times)
                : ::utimensat(AT_FDCWD, metadata.host_path.c_str(), times,
                      AT_SYMLINK_NOFOLLOW);
        if (result != 0)
            return false;
    }
    // Subsequent reads come from the inode, so host edits and checkpoints see
    // the same attributes. Keep only facts that the host cannot represent.
    update.mode.reset();
    update.owner.reset();
    update.group.reset();
    update.flags.reset();
    update.data_protection_class.reset();
    update.creation_time.reset();
    update.backup_time.reset();
    update.modification_time.reset();
    update.access_time.reset();
    update.finder_info.reset();
    update.resource_length.reset();
    update.resource_allocation_size.reset();
    return true;
}

bool MetadataProvider::persist_named_attribute(const Metadata& metadata,
    std::string_view name, const std::optional<std::vector<std::byte>>& value)
{
    if (metadata.object_type == 5U || name.starts_with("hfsfuse.record."))
        return false;
    const auto key = std::string { "user." } + std::string { name };
    // Resource forks already use a sidecar; remove a legacy inline copy so a
    // guest deletion cannot uncover stale firmware data on the next boot.
    if (value && name != "com.apple.ResourceFork")
        return write_attribute(metadata, key, value->data(), value->size());
    const auto result =
        metadata.host_descriptor >= 0
            ? ::fremovexattr(metadata.host_descriptor, key.c_str())
            : ::lremovexattr(metadata.host_path.c_str(), key.c_str());
    return result == 0 || errno == ENODATA;
}
} // namespace ilemu::hfs
