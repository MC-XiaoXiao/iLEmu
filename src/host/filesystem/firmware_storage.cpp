// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
#include "host/firmware_storage.hpp"
#include "filesystem/hfs_metadata.hpp"

#include <cerrno>
#include <fcntl.h>
#include <stdexcept>
#include <sys/stat.h>
#include <sys/xattr.h>
#include <system_error>
#include <unistd.h>

namespace ilemu {
namespace {
    struct stat host_status(const std::filesystem::path& path)
    {
        struct stat result { };
        if (::lstat(path.c_str(), &result) != 0)
            throw std::system_error(
                errno, std::generic_category(), path.string());
        return result;
    }

    void accessible(const std::filesystem::path& path, const struct stat& info)
    {
        if (S_ISLNK(info.st_mode))
            return;
        // Add owner access before reading xattrs or descending into legacy 0000
        // directories. Do not copy guest set-id, sticky or immutable flags.
        const auto mode = S_ISDIR(info.st_mode) ? 0700U : 0600U;
        if ((info.st_mode & 07777U) != mode && ::chmod(path.c_str(), mode) != 0)
            throw std::system_error(
                errno, std::generic_category(), path.string());
    }

    void numeric(
        FirmwareStorage::Attributes& values, const char* name, unsigned value)
    {
        const auto text = std::to_string(value);
        const auto* first = reinterpret_cast<const std::byte*>(text.c_str());
        values.try_emplace(std::string { "user.hfsfuse.record." } + name, first,
            first + text.size() + 1);
    }
}

namespace {
    FirmwareStorage::Attributes read_attributes(
        const std::filesystem::path& path, int descriptor)
    {
        FirmwareStorage::Attributes result;
        const auto list = [&](char* names, std::size_t size) {
            return descriptor >= 0 ? ::flistxattr(descriptor, names, size)
                                   : ::llistxattr(path.c_str(), names, size);
        };
        const auto read = [&](const char* name, void* bytes, std::size_t size) {
            return descriptor >= 0
                       ? ::fgetxattr(descriptor, name, bytes, size)
                       : ::lgetxattr(path.c_str(), name, bytes, size);
        };
        const auto length = list(nullptr, 0);
        if (length < 0)
            throw std::system_error(errno, std::generic_category(),
                "read firmware attributes: " + path.string());
        std::vector<char> names(static_cast<std::size_t>(length));
        const auto received = list(names.data(), names.size());
        if (received < 0)
            throw std::system_error(
                errno, std::generic_category(), path.string());
        for (std::size_t offset = 0;
            offset < static_cast<std::size_t>(received);) {
            const std::string name { names.data() + offset };
            offset += name.size() + 1;
            if (!name.starts_with("user."))
                continue;
            const auto size = read(name.c_str(), nullptr, 0);
            if (size < 0)
                throw std::system_error(
                    errno, std::generic_category(), path.string());
            auto& value = result[name];
            value.resize(static_cast<std::size_t>(size));
            if (read(name.c_str(), value.data(), value.size()) != size)
                throw std::system_error(
                    errno, std::generic_category(), path.string());
        }
        return result;
    }

    void write_attributes(const std::filesystem::path& path,
        const FirmwareStorage::Attributes& values, int descriptor)
    {
        for (const auto& [name, value] : values) {
            if (!name.starts_with("user.") ||
                name.find('\0') != std::string::npos)
                throw std::runtime_error("invalid firmware attribute name");
            const auto written = descriptor >= 0
                                     ? ::fsetxattr(descriptor, name.c_str(),
                                           value.data(), value.size(), 0)
                                     : ::lsetxattr(path.c_str(), name.c_str(),
                                           value.data(), value.size(), 0);
            if (written != 0)
                throw std::system_error(errno, std::generic_category(),
                    "preserve guest attributes: " + path.string());
        }
    }

} // namespace

FirmwareStorage::Attributes FirmwareStorage::attributes(
    const std::filesystem::path& path)
{
    return read_attributes(path, -1);
}

void FirmwareStorage::attributes(
    const std::filesystem::path& path, const Attributes& values)
{
    write_attributes(path, values, -1);
}

void FirmwareStorage::normalize(
    const std::filesystem::path& path, Attributes values)
{
    // Pin the inode before touching permissions or guest metadata. An editor
    // may atomically replace the pathname while preparation is in progress.
    int descriptor =
        ::open(path.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK);
    struct stat info { };
    if (descriptor < 0 && errno == ELOOP)
        return; // Guest symlinks are never followed on the host.
    if (descriptor < 0 && errno == EACCES) {
        // O_PATH can pin legacy mode-0000 entries without read permission.
        // /proc/self/fd refers to that pinned object even after a rename.
        const auto anchor =
            ::open(path.c_str(), O_PATH | O_NOFOLLOW | O_CLOEXEC);
        if (anchor < 0)
            throw std::system_error(
                errno, std::generic_category(), path.string());
        const auto anchored =
            std::string { "/proc/self/fd/" } + std::to_string(anchor);
        if (::fstat(anchor, &info) != 0 ||
            (!S_ISREG(info.st_mode) && !S_ISDIR(info.st_mode))) {
            const auto error = errno;
            ::close(anchor);
            if (S_ISLNK(info.st_mode))
                return;
            throw std::system_error(
                error ? error : EINVAL, std::generic_category(), path.string());
        }
        if (::chmod(anchored.c_str(), S_ISDIR(info.st_mode) ? 0700U : 0600U) ==
            0)
            descriptor =
                ::open(anchored.c_str(), O_RDONLY | O_CLOEXEC | O_NONBLOCK);
        const auto error = errno;
        if (descriptor < 0)
            ::chmod(anchored.c_str(), info.st_mode & 07777U);
        ::close(anchor);
        if (descriptor < 0)
            throw std::system_error(
                error, std::generic_category(), path.string());
    } else if (descriptor >= 0 && ::fstat(descriptor, &info) != 0) {
        const auto error = errno;
        ::close(descriptor);
        throw std::system_error(error, std::generic_category(), path.string());
    }
    if (descriptor < 0)
        throw std::system_error(errno, std::generic_category(), path.string());
    struct PinnedFile {
        int descriptor;
        ~PinnedFile() { ::close(descriptor); }
    } pinned { descriptor };
    if (!S_ISREG(info.st_mode) && !S_ISDIR(info.st_mode))
        throw std::runtime_error(
            "unsupported firmware file type: " + path.string());
    try {
        const auto existing = read_attributes(path, descriptor);
        for (const auto& [name, value] : existing)
            values.try_emplace(name, value);
        numeric(values, "file_mode", info.st_mode);
        const auto mode = S_ISDIR(info.st_mode) ? 0700U : 0600U;
        if ((info.st_mode & 07777U) != mode && ::fchmod(descriptor, mode) != 0)
            throw std::system_error(
                errno, std::generic_category(), path.string());
        for (const auto& [name, value] : existing) {
            if (const auto found = values.find(name);
                found != values.end() && found->second == value)
                values.erase(found);
        }
        write_attributes(path, values, descriptor);
    } catch (...) {
        ::fchmod(descriptor, info.st_mode & 07777U);
        throw;
    }
}

void FirmwareStorage::prepare(const std::filesystem::path& root)
{
    normalize(root);
    if (!S_ISDIR(host_status(root).st_mode))
        return;
    for (const auto& child : std::filesystem::directory_iterator(root)) {
        try {
            const auto info = host_status(child.path());
            if (S_ISDIR(info.st_mode))
                prepare(child.path());
            else if (S_ISREG(info.st_mode))
                normalize(child.path());
        } catch (const std::system_error& error) {
            if (error.code() != std::errc::no_such_file_or_directory)
                throw;
        }
        // Symlinks (including absolute guest paths), runtime FIFOs and sockets
        // must never be followed or changed on the host.
    }
}

void FirmwareStorage::remove(const std::filesystem::path& root)
{
    struct stat info { };
    if (::lstat(root.c_str(), &info) != 0) {
        if (errno == ENOENT)
            return;
        throw std::system_error(errno, std::generic_category(), root.string());
    }
    if (S_ISDIR(info.st_mode)) {
        accessible(root, info);
        for (const auto& child : std::filesystem::directory_iterator(root))
            remove(child.path());
    }
    if (::remove(root.c_str()) != 0)
        throw std::system_error(errno, std::generic_category(), root.string());
}

void FirmwareStorage::copy(const std::filesystem::path& source,
    const std::filesystem::path& destination)
{
    const auto from = std::filesystem::weakly_canonical(source);
    const auto to = std::filesystem::weakly_canonical(destination);
    const auto relative = to.lexically_relative(from);
    if (to == from || (!relative.empty() && *relative.begin() != ".."))
        throw std::runtime_error(
            "firmware destination must be outside the source tree");
    if (std::filesystem::symlink_status(destination).type() !=
        std::filesystem::file_type::not_found)
        throw std::runtime_error("firmware destination already exists");
    // Keep the source read-only; explicit preparation can recover inaccessible
    // legacy sources before copying them.
    std::function<void(
        const std::filesystem::path&, const std::filesystem::path&)>
        append;
    std::map<std::pair<dev_t, ino_t>, std::filesystem::path> links;
    append = [&](const auto& input, const auto& output) {
        const auto info = host_status(input);
        auto values =
            S_ISLNK(info.st_mode) ? Attributes { } : attributes(input);
        numeric(values, "file_mode", info.st_mode);
        if (!S_ISLNK(info.st_mode)) {
            const auto guest =
                hfs::MetadataProvider { input.root_path() }.query(
                    input, false, false);
            if (guest) {
                numeric(values, "owner_id", guest->owner);
                numeric(values, "group_id", guest->group);
            }
        }
        if (S_ISLNK(info.st_mode)) {
            std::filesystem::create_symlink(
                std::filesystem::read_symlink(input), output);
            return;
        }
        if (S_ISDIR(info.st_mode)) {
            std::filesystem::create_directory(output);
            normalize(output, values);
            for (const auto& child : std::filesystem::directory_iterator(input))
                append(child.path(), output / child.path().filename());
        } else if (S_ISREG(info.st_mode)) {
            if (std::filesystem::space(output.parent_path()).available <
                static_cast<std::uint64_t>(info.st_size) + 64U * 1024U * 1024U)
                throw std::runtime_error(
                    "insufficient space for firmware copy");
            const auto key = std::pair { info.st_dev, info.st_ino };
            const auto previous = links.find(key);
            std::error_code error;
            if (previous != links.end())
                std::filesystem::create_hard_link(
                    previous->second, output, error);
            if (previous == links.end() || error) {
                if (error && error != std::errc::operation_not_permitted &&
                    error != std::errc::permission_denied &&
                    error != std::errc::operation_not_supported)
                    throw std::system_error(error, "copy firmware hard link");
                std::filesystem::copy_file(input, output);
                normalize(output, values);
                links.try_emplace(key, output);
            }
        } else {
            return; // Runtime sockets/FIFOs are recreated by the firmware.
        }
        std::filesystem::last_write_time(
            output, std::filesystem::last_write_time(input));
    };
    try {
        append(source, destination);
    } catch (...) {
        remove(destination);
        throw;
    }
}

} // namespace ilemu
