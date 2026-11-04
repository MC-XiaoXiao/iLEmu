// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "filesystem/hfs_metadata.hpp"

#include <limits>
#include <mutex>
#include <system_error>
#include <unordered_map>

namespace ilemu::hfs {

struct MetadataProvider::CatalogIndex {
    std::mutex mutex;
    std::unordered_map<std::uint32_t, std::filesystem::path> paths;
};

MetadataProvider::MetadataProvider(std::filesystem::path root)
    : root_ { std::move(root) }
    , catalog_ { std::make_shared<CatalogIndex>() }
{
}

void MetadataProvider::remember_catalog_path(std::uint32_t catalog_id,
    std::uint32_t permanent_id, const std::filesystem::path& path) const
{
    const std::lock_guard lock { catalog_->mutex };
    catalog_->paths.insert_or_assign(catalog_id, path);
    if (permanent_id != catalog_id)
        catalog_->paths.insert_or_assign(permanent_id, path);
}

std::optional<std::filesystem::path> MetadataProvider::path_for_catalog_id(
    std::uint64_t object_id) const
{
    if (object_id == 2)
        return root_;
    if (object_id > std::numeric_limits<std::uint32_t>::max())
        return std::nullopt;

    const auto id = static_cast<std::uint32_t>(object_id);
    std::optional<std::filesystem::path> observed;
    {
        const std::lock_guard lock { catalog_->mutex };
        if (const auto found = catalog_->paths.find(id); found != catalog_->paths.end())
            observed = found->second;
    }
    if (observed) {
        // Renames and deletion may invalidate the index between calls.
        const auto current = query_directory_entry(*observed, false);
        if (current && (current->catalog_id == id || current->permanent_id == id))
            return observed;
    }

    // The extracted filesystem is our catalog backend. Reuse exactly the
    // identity calculation used by getattrlist and directory enumeration.
    // Do not follow symlinks: their targets can leave this filesystem.
    std::error_code error;
    std::filesystem::recursive_directory_iterator entry { root_,
        std::filesystem::directory_options::skip_permission_denied, error };
    const std::filesystem::recursive_directory_iterator end;
    while (!error && entry != end) {
        if (!is_resource_sidecar(entry->path())) {
            if (const auto metadata = query_directory_entry(entry->path(), false);
                metadata && (metadata->catalog_id == id || metadata->permanent_id == id))
                return entry->path();
        }
        entry.increment(error);
    }
    return std::nullopt;
}

} // namespace ilemu::hfs
