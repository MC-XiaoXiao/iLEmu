#include "runtime/boot_options.hpp"

namespace ilemu {

std::filesystem::path default_host_cache_directory(
    const std::filesystem::path& rootfs)
{
    const auto normalized = rootfs.lexically_normal();
    auto rootfs_name = normalized.filename();
    if (rootfs_name.empty() || rootfs_name == "." ||
        rootfs_name == normalized.root_name()) {
        rootfs_name = "rootfs";
    }
    return normalized.parent_path() / ".ilegacysim-cache" / rootfs_name;
}

} // namespace ilemu
