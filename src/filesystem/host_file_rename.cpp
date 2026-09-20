// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
#include "filesystem/host_file_rename.hpp"
#include "filesystem/hfs_metadata.hpp"
#include <utility>

namespace ilemu {
namespace {
    void rename_host_file(const std::filesystem::path& root,
        const std::filesystem::path& source,
        const std::filesystem::path& destination, HostFileRenameResult& result)
    {
        hfs::MetadataProvider metadata { root };
        const auto original = metadata.query(source, false, false);
        const auto replaced = metadata.query(destination, false, false);
        if (original && replaced &&
            original->permanent_id == replaced->permanent_id)
            return; // POSIX same-inode rename is a no-op, including
                    // sidecars.
        result.directory = original && original->directory;
        std::filesystem::rename(source, destination, result.error);
        if (result.error)
            return;
        result.renamed = true;
        if (replaced)
            result.replaced_identity = replaced->permanent_id;
        const auto source_resource =
            hfs::MetadataProvider::resource_sidecar(source);
        const auto destination_resource =
            hfs::MetadataProvider::resource_sidecar(destination);
        std::error_code error;
        if (std::filesystem::is_regular_file(source_resource, error)) {
            error.clear();
            static_cast<void>(
                std::filesystem::remove(destination_resource, error));
            error.clear();
            std::filesystem::rename(
                source_resource, destination_resource, error);
        } else {
            error.clear();
            static_cast<void>(
                std::filesystem::remove(destination_resource, error));
            error.clear();
        }
        result.error = error;
        return;
    }
} // namespace

std::optional<HostFileRenameResult> HostFileRenameRequest::result() const
{
    if (!finished_.load(std::memory_order_acquire))
        return std::nullopt;
    return result_;
}
HostFileRenamer::~HostFileRenamer() { worker_.wait_idle(); }
std::shared_ptr<HostFileRenameRequest> HostFileRenamer::rename(
    std::filesystem::path root, std::filesystem::path source,
    std::filesystem::path destination)
{
    auto request = std::make_shared<HostFileRenameRequest>();
    const auto task = worker_.submit(HostWorkKind::Maintenance, std::nullopt,
        [request, root = std::move(root), source = std::move(source),
            destination = std::move(destination)] {
            try {
                rename_host_file(root, source, destination, request->result_);
            } catch (const std::system_error& error) {
                request->result_.error = error.code();
            } catch (...) {
                request->result_.error =
                    std::make_error_code(std::errc::io_error);
            }
            request->finished_.store(true, std::memory_order_release);
        });
    if (!task) {
        request->result_.error = std::make_error_code(std::errc::io_error);
        request->finished_.store(true, std::memory_order_release);
    }
    return request;
}
} // namespace ilemu
