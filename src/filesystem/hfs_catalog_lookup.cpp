// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "filesystem/hfs_metadata.hpp"

#include <limits>
#include <system_error>

namespace ilemu::hfs {

std::optional<std::filesystem::path> MetadataProvider::path_for_catalog_id(
    std::uint64_t object_id) const
{
    if (object_id == 2)
        return root_;
    if (object_id > std::numeric_limits<std::uint32_t>::max())
        return std::nullopt;

    const auto id = static_cast<std::uint32_t>(object_id);
    const std::lock_guard lock { catalog_mutex_ };
    if (const auto found = catalog_paths_.find(id); found != catalog_paths_.end()) {
        // Renames and deletion may invalidate the index between calls.
        const auto current = query_directory_entry(found->second, false);
        if (current && current->catalog_id == id)
            return found->second;
    }

    // The extracted filesystem is our catalog backend. Reuse exactly the
    // identity calculation used by getattrlist and directory enumeration.
    // Do not follow symlinks: their targets can leave this filesystem.
    catalog_paths_.clear();
    std::error_code error;
    std::filesystem::recursive_directory_iterator entry { root_,
        std::filesystem::directory_options::skip_permission_denied, error };
    const std::filesystem::recursive_directory_iterator end;
    while (!error && entry != end) {
        if (!is_resource_sidecar(entry->path())) {
            if (const auto metadata = query_directory_entry(entry->path(), false))
                catalog_paths_.try_emplace(metadata->catalog_id, entry->path());
        }
        entry.increment(error);
    }
    if (const auto found = catalog_paths_.find(id); found != catalog_paths_.end())
        return found->second;
    return std::nullopt;
}

} // namespace ilemu::hfs
