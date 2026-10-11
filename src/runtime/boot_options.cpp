// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// Define session startup options and resolve runtime cache defaults.

#include "runtime/boot_options.hpp"
#include <stdexcept>

namespace ilemu {

std::vector<std::string> BootOptions::initial_arguments() const
{
    if (guest_command) {
        if (!guest_arguments.empty())
            throw std::invalid_argument(
                "guest arguments cannot be combined with --guest-command");
        return { binary, "-c", *guest_command };
    }
    std::vector<std::string> result { binary };
    result.insert(result.end(), guest_arguments.begin(), guest_arguments.end());
    return result;
}

std::filesystem::path default_host_cache_directory(
    const std::filesystem::path& rootfs)
{
    const auto normalized = rootfs.lexically_normal();
    auto rootfs_name = normalized.filename();
    if (rootfs_name.empty() || rootfs_name == "." ||
        rootfs_name == normalized.root_name()) {
        rootfs_name = "rootfs";
    }
    return normalized.parent_path() / ".ilemu-cache" / rootfs_name;
}

} // namespace ilemu
