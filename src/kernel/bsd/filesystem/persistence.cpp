// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// Handle guest file synchronization and persistence requests.
//
// Apple public ABI/behavior references (guest profiles may differ):
// https://github.com/apple-oss-distributions/xnu/blob/xnu-792.24.17/bsd/vfs/vfs_syscalls.c

#include "kernel/kernel.hpp"

#include "foundation/host_file_sync.hpp"

#include "kernel/darwin_abi.hpp"

#include <memory>
#include <mutex>
#include <string>
#include <system_error>
#include <utility>

#include "../support.hpp"

namespace ilemu {

void CompatibilityKernel::persist_hfs_metadata(
    const hfs::Metadata& metadata, hfs::MetadataOverride& update)
{
    const auto relative = metadata.host_path.lexically_relative(rootfs_);
    if (relative.empty() || *relative.begin() == "..")
        return;
    const auto guest =
        (std::filesystem::path { "/" } / relative).generic_string();
    if ((hfs_volumes_.for_guest_path(guest).mount_flags & 1U) != 0)
        return; // Preserve the system partition's existing virtual overlay.
    if (!hfs::MetadataProvider::persist_override(metadata, update)) {
        if (metadata.object_type != 5U)
            output_.write(
                "[vfs] could not persist guest metadata: " + guest + "\n");
        return;
    }
    if (const auto inode = shared_state_->hfs_named_attribute_overrides.find(
            metadata.host_identity);
        inode != shared_state_->hfs_named_attribute_overrides.end()) {
        for (auto attribute = inode->second.begin();
            attribute != inode->second.end();) {
            if (hfs::MetadataProvider::persist_named_attribute(
                    metadata, attribute->first, attribute->second))
                attribute = inode->second.erase(attribute);
            else
                ++attribute;
        }
    }
    if (const auto current = hfs_metadata_.query(
            metadata.host_path, true, false, metadata.host_descriptor)) {
        const auto change_time = update.change_time;
        update.observe(*current);
        update.change_time = change_time;
    }
}

bool CompatibilityKernel::dispatch_bsd_filesystem_persistence(
    Cpu& cpu, std::uint32_t number)
{
    if (number != darwin::syscall::synchronize_file)
        return false;

    auto fd = cpu.registers()[0];
    if (const auto duplicate = duplicated_descriptors_.find(fd);
        duplicate != duplicated_descriptors_.end()) {
        fd = duplicate->second;
    }
    if (auto operation = prepare_file_io(cpu, fd,
            FileIoOperation::Kind::Synchronize, 0, 0))
        wait_for_file_io(cpu, std::move(operation));
    return true;
}

} // namespace ilemu
