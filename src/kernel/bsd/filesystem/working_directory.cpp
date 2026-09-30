// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// Process and per-thread working directories, using the shared VFS resolver.
// https://github.com/apple-oss-distributions/xnu/blob/xnu-3248.60.10/bsd/vfs/vfs_syscalls.c

#include "kernel/kernel.hpp"

#include "../support.hpp"

namespace ilemu {

const std::filesystem::path& CompatibilityKernel::working_directory(
    std::optional<std::size_t> processor) const
{
    if (processor) {
        const auto directory = thread_working_directories_.find(*processor);
        if (directory != thread_working_directories_.end())
            return directory->second;
    }
    return guest_working_directory_;
}

std::filesystem::path CompatibilityKernel::resolve_guest_path(const Cpu& cpu,
    const std::string& path, bool follow_final_symlink) const
{
    return resolve_guest_path(path, follow_final_symlink, cpu.processor_id());
}

bool CompatibilityKernel::dispatch_bsd_directory(Cpu& cpu, std::uint32_t number)
{
    const bool per_thread = number == 348U || number == 349U;
    const bool by_descriptor = number == 13U || number == 349U;
    if (!per_thread && number != 12U && number != 13U)
        return false;

    const auto argument = cpu.registers()[0];
    if (per_thread && by_descriptor && argument == 0xffffffffU) {
        if (thread_working_directories_.erase(cpu.processor_id()) != 0U)
            bsd_success(cpu, 0);
        else
            bsd_error(cpu, bsd_support::bad_file_descriptor);
        return true;
    }

    std::filesystem::path host;
    if (by_descriptor) {
        auto fd = argument;
        if (const auto duplicate = duplicated_descriptors_.find(fd);
            duplicate != duplicated_descriptors_.end())
            fd = duplicate->second;
        const auto descriptor = file_descriptors_.find(fd);
        if (descriptor == file_descriptors_.end()) {
            bsd_error(cpu, bsd_support::bad_file_descriptor);
            return true;
        }
        host = descriptor->second;
    } else {
        const auto path = memory_.read_c_string(argument);
        if (!path || path->empty()) {
            bsd_error(cpu, path ? 2U : bsd_support::bad_address);
            return true;
        }
        host = resolve_guest_path(cpu, *path);
    }

    std::error_code error;
    const auto status = std::filesystem::status(host, error);
    if (error || !std::filesystem::is_directory(status)) {
        bsd_error(cpu, error ? bsd_support::darwin_filesystem_error(error, 2U)
                            : 20U); // ENOTDIR
        return true;
    }
    // Retain the resolved directory so relative lookups after chdir through a
    // symlink use its target, just as XNU retains the directory vnode.
    const auto relative = host.lexically_normal().lexically_relative(
        rootfs_.lexically_normal());
    if (relative.empty() || *relative.begin() == "..") {
        bsd_error(cpu, 2U);
        return true;
    }
    const auto guest = (std::filesystem::path { "/" } / relative).lexically_normal();
    if (per_thread)
        thread_working_directories_.insert_or_assign(cpu.processor_id(), guest);
    else
        guest_working_directory_ = guest;
    output_.line("[vfs] " + std::string { per_thread ? "thread-cwd" : "cwd" } +
        " pid=" + std::to_string(process_.pid) + " slot=" +
        std::to_string(cpu.processor_id()) + " path=" + guest.string());
    bsd_success(cpu, 0);
    return true;
}

} // namespace ilemu
