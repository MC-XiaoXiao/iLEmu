// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
#include "filesystem/hfs_metadata.hpp"
#include "host/firmware_storage.hpp"

#include <archive.h>
#include <archive_entry.h>
#include <array>
#include <cerrno>
#include <charconv>
#include <exception>
#include <fstream>
#include <memory>
#include <set>
#include <stdexcept>
#include <sys/stat.h>
#include <system_error>

namespace ilemu {
namespace {
    using Archive = std::unique_ptr<archive, decltype(&archive_free)>;
    using Entry = std::unique_ptr<archive_entry, decltype(&archive_entry_free)>;

    void checked(archive* handle, la_ssize_t result)
    {
        if (result < ARCHIVE_OK)
            throw std::runtime_error(archive_error_string(handle)
                                         ? archive_error_string(handle)
                                         : "firmware archive failed");
    }

    std::filesystem::path relative_path(const char* name)
    {
        if (!name)
            throw std::runtime_error("missing firmware archive path");
        std::filesystem::path path { name };
        if (path.empty())
            throw std::runtime_error("invalid firmware archive path");
        for (const auto& part : path) {
            if (part == "..")
                throw std::runtime_error(
                    "firmware archive path escapes its root");
        }
        // hfstar also emits volume-rooted paths (/usr, /var). Anchor them in
        // the selected firmware root rather than the host filesystem.
        path = path.relative_path().lexically_normal();
        return path.empty() ? std::filesystem::path { "." } : path;
    }

    void numeric(FirmwareStorage::Attributes& values, const char* name,
        std::uint64_t value)
    {
        const auto text = std::to_string(value);
        const auto* first = reinterpret_cast<const std::byte*>(text.c_str());
        values.try_emplace(std::string { "user.hfsfuse.record." } + name, first,
            first + text.size() + 1);
    }

    FirmwareStorage::Attributes guest_attributes(archive_entry* entry)
    {
        FirmwareStorage::Attributes values;
        archive_entry_xattr_reset(entry);
        const char* name;
        const void* bytes;
        std::size_t size;
        while (archive_entry_xattr_next(entry, &name, &bytes, &size) ==
               ARCHIVE_OK) {
            if (!std::string_view { name }.starts_with("user."))
                continue;
            const auto* first = static_cast<const std::byte*>(bytes);
            values[name] = std::vector<std::byte>(first, first + size);
        }
        numeric(values, "file_mode", archive_entry_mode(entry));
        numeric(values, "owner_id", archive_entry_uid(entry));
        numeric(values, "group_id", archive_entry_gid(entry));
        return values;
    }

    void number_from_attribute(archive_entry* entry,
        const FirmwareStorage::Attributes& values, const char* key,
        void (*set)(archive_entry*, la_int64_t))
    {
        const auto found =
            values.find(std::string { "user.hfsfuse.record." } + key);
        if (found == values.end())
            return;
        const auto* first = reinterpret_cast<const char*>(found->second.data());
        const auto* last = first + found->second.size();
        if (last != first && last[-1] == '\0')
            --last;
        std::uint32_t number { };
        const auto result = std::from_chars(first, last, number);
        if (result.ec == std::errc { } && result.ptr == last)
            set(entry, number);
    }

    struct Input {
        FirmwareArchive::Reader read;
        std::array<std::byte, 64 * 1024> bytes;
        std::exception_ptr error;
        static la_ssize_t next(
            archive*, void* opaque, const void** data) noexcept
        {
            auto& input = *static_cast<Input*>(opaque);
            try {
                *data = input.bytes.data();
                return static_cast<la_ssize_t>(
                    input.read(input.bytes.data(), input.bytes.size()));
            } catch (...) {
                input.error = std::current_exception();
                return -1;
            }
        }
        void rethrow() const
        {
            if (error)
                std::rethrow_exception(error);
        }
    };
    struct Output {
        FirmwareArchive::Writer write;
        std::exception_ptr error;
        static la_ssize_t next(
            archive*, void* opaque, const void* data, std::size_t size) noexcept
        {
            auto& output = *static_cast<Output*>(opaque);
            try {
                output.write(data, size);
                return static_cast<la_ssize_t>(size);
            } catch (...) {
                output.error = std::current_exception();
                return -1;
            }
        }
        void rethrow() const
        {
            if (error)
                std::rethrow_exception(error);
        }
    };
}

void FirmwareArchive::unpack(Reader read, const std::filesystem::path& root)
{
    const auto requested = std::filesystem::absolute(root).lexically_normal();
    if (std::filesystem::is_symlink(std::filesystem::symlink_status(requested)))
        throw std::runtime_error("firmware root is a symlink");
    // Resolve host storage aliases before libarchive's symlink checks. Android
    // app-data paths can themselves have symlinked ancestors (/data/user/0).
    // Links inside the archive still go through the secure extraction policy.
    const auto base = std::filesystem::weakly_canonical(requested);
    std::filesystem::create_directories(base);
    FirmwareStorage::normalize(base);
    Input input { std::move(read), { }, { } };
    Archive source { archive_read_new(), archive_free };
    Archive disk { archive_write_disk_new(), archive_free };
    checked(source.get(), archive_read_support_format_tar(source.get()));
    checked(source.get(), archive_read_support_filter_all(source.get()));
    checked(disk.get(), archive_write_disk_set_options(disk.get(),
                            ARCHIVE_EXTRACT_TIME | ARCHIVE_EXTRACT_PERM |
                                ARCHIVE_EXTRACT_SECURE_SYMLINKS |
                                ARCHIVE_EXTRACT_SECURE_NODOTDOT));
    const auto opened =
        archive_read_open(source.get(), &input, nullptr, Input::next, nullptr);
    input.rethrow();
    checked(source.get(), opened);
    std::vector<Entry> links;
    std::set<std::filesystem::path> seen;
    std::size_t count = 0;
    archive_entry* entry;
    while (true) {
        const auto next = archive_read_next_header(source.get(), &entry);
        input.rethrow();
        if (next == ARCHIVE_EOF)
            break;
        checked(source.get(), next);
        if (++count > 250'000)
            throw std::runtime_error("too many firmware archive entries");
        const auto relative = relative_path(archive_entry_pathname(entry));
        if (!seen.emplace(relative).second)
            throw std::runtime_error(
                "duplicate firmware archive entry: " + relative.string());
        const auto path = base / relative;
        if (const auto target = archive_entry_hardlink(entry)) {
            auto copy =
                Entry { archive_entry_clone(entry), archive_entry_free };
            const auto source_path = base / relative_path(target);
            archive_entry_set_pathname(copy.get(), path.c_str());
            archive_entry_set_hardlink(copy.get(), source_path.c_str());
            links.push_back(std::move(copy));
            continue;
        }
        const auto type = archive_entry_filetype(entry);
        if (type != AE_IFREG && type != AE_IFDIR && type != AE_IFLNK)
            throw std::runtime_error("unsupported firmware archive file type");
        if (relative == "." && type != AE_IFDIR)
            throw std::runtime_error(
                "firmware archive root must be a directory");
        if (type == AE_IFREG &&
            (archive_entry_size(entry) < 0 ||
                std::filesystem::space(base).available <
                    static_cast<std::uint64_t>(archive_entry_size(entry)) +
                        64U * 1024U * 1024U))
            throw std::runtime_error("insufficient space for firmware archive");
        const auto values = guest_attributes(entry);
        archive_entry_set_pathname(entry, path.c_str());
        archive_entry_set_perm(entry, type == AE_IFDIR ? 0700 : 0600);
        archive_entry_set_fflags(entry, 0, 0);
        archive_entry_acl_clear(entry);
        archive_entry_xattr_clear(entry);
        checked(disk.get(), archive_write_header(disk.get(), entry));
        const void* data;
        std::size_t size;
        la_int64_t offset;
        while (true) {
            const auto block =
                archive_read_data_block(source.get(), &data, &size, &offset);
            input.rethrow();
            if (block == ARCHIVE_EOF)
                break;
            checked(source.get(), block);
            checked(disk.get(),
                archive_write_data_block(disk.get(), data, size, offset));
        }
        checked(disk.get(), archive_write_finish_entry(disk.get()));
        if (type != AE_IFLNK)
            FirmwareStorage::attributes(path, values);
    }
    // POSIX tar can contain forward hard links. Let libarchive validate paths
    // and create the links once all ordinary entries exist.
    for (std::size_t unresolved = links.size(); unresolved != 0;) {
        const auto before = unresolved;
        for (auto& link : links) {
            if (!link)
                continue;
            const auto target =
                std::filesystem::path { archive_entry_hardlink(link.get()) };
            // Reject symlinks in every component of the link source, including
            // the final component. Guest symlink targets are never host paths.
            const auto path =
                std::filesystem::path { archive_entry_pathname(link.get()) };
            for (const auto& checked_path : { target, path }) {
                auto current = base;
                for (const auto& part : checked_path.lexically_relative(base)) {
                    current /= part;
                    if (std::filesystem::is_symlink(
                            std::filesystem::symlink_status(current)))
                        throw std::runtime_error(
                            "firmware hard link traverses a symlink");
                }
            }
            if (!std::filesystem::is_regular_file(target))
                continue;
            archive_entry_set_perm(link.get(), 0600);
            archive_entry_set_fflags(link.get(), 0, 0);
            archive_entry_acl_clear(link.get());
            archive_entry_xattr_clear(link.get());
            const auto result = archive_write_header(disk.get(), link.get());
            if (result < ARCHIVE_OK) {
                // Android SELinux may deny link(2) within app data. Reuse the
                // file copy path and its guest-attribute preservation.
                const auto error = archive_errno(disk.get());
                if (error != EACCES && error != EPERM && error != EOPNOTSUPP)
                    checked(disk.get(), result);
                FirmwareStorage::copy(target, path);
            } else {
                checked(disk.get(), archive_write_finish_entry(disk.get()));
            }
            link.reset();
            --unresolved;
        }
        if (before == unresolved)
            throw std::runtime_error("unresolved firmware archive hard links");
    }
    checked(disk.get(), archive_write_close(disk.get()));
    checked(source.get(), archive_read_close(source.get()));
    FirmwareStorage::prepare(base);
}

void FirmwareArchive::pack(const std::filesystem::path& root, Writer write)
{
    Output output { std::move(write), { } };
    Archive archive { archive_write_new(), archive_free };
    checked(archive.get(), archive_write_set_format_pax(archive.get()));
    const auto opened = archive_write_open(
        archive.get(), &output, nullptr, Output::next, nullptr);
    output.rethrow();
    checked(archive.get(), opened);
    std::map<std::pair<dev_t, ino_t>, std::string> links;
    std::function<void(const std::filesystem::path&, const std::string&)>
        append;
    append = [&](const auto& path, const auto& name) {
        struct stat info { };
        if (::lstat(path.c_str(), &info) != 0)
            throw std::system_error(
                errno, std::generic_category(), path.string());
        if (!S_ISREG(info.st_mode) && !S_ISDIR(info.st_mode) &&
            !S_ISLNK(info.st_mode))
            return;
        Entry entry { archive_entry_new(), archive_entry_free };
        archive_entry_copy_stat(entry.get(), &info);
        archive_entry_set_pathname(entry.get(), name.c_str());
        const auto guest = hfs::MetadataProvider { root.root_path() }.query(
            path, false, false);
        archive_entry_set_uid(entry.get(), guest ? guest->owner : 0);
        archive_entry_set_gid(entry.get(), guest ? guest->group : 0);
        if (S_ISLNK(info.st_mode)) {
            archive_entry_set_symlink(
                entry.get(), std::filesystem::read_symlink(path).c_str());
        } else {
            const auto values = FirmwareStorage::attributes(path);
            for (const auto& [key, value] : values)
                archive_entry_xattr_add_entry(
                    entry.get(), key.c_str(), value.data(), value.size());
            number_from_attribute(
                entry.get(), values, "owner_id", archive_entry_set_uid);
            number_from_attribute(
                entry.get(), values, "group_id", archive_entry_set_gid);
            number_from_attribute(entry.get(), values, "file_mode",
                [](archive_entry* e, la_int64_t mode) {
                    archive_entry_set_perm(
                        e, static_cast<mode_t>(mode) & 07777U);
                });
        }
        const auto identity = std::pair { info.st_dev, info.st_ino };
        bool data = S_ISREG(info.st_mode);
        if (data && info.st_nlink > 1) {
            if (const auto previous = links.find(identity);
                previous != links.end()) {
                archive_entry_set_hardlink(
                    entry.get(), previous->second.c_str());
                data = false;
            } else {
                links.emplace(identity, name);
            }
        }
        if (!data)
            archive_entry_set_size(entry.get(), 0);
        checked(
            archive.get(), archive_write_header(archive.get(), entry.get()));
        if (data) {
            std::ifstream stream { path, std::ios::binary };
            if (!stream)
                throw std::runtime_error(
                    "cannot read firmware file: " + path.string());
            std::array<char, 64 * 1024> bytes;
            while (stream) {
                stream.read(bytes.data(), bytes.size());
                const auto count = static_cast<std::size_t>(stream.gcount());
                const auto written =
                    archive_write_data(archive.get(), bytes.data(), count);
                output.rethrow();
                if (written < 0 || static_cast<std::size_t>(written) != count)
                    throw std::runtime_error("firmware archive write failed");
            }
            if (!stream.eof())
                throw std::runtime_error("firmware file read failed");
        }
        checked(archive.get(), archive_write_finish_entry(archive.get()));
        output.rethrow();
        if (S_ISDIR(info.st_mode)) {
            for (const auto& child : std::filesystem::directory_iterator(path))
                append(child.path(),
                    name == "."
                        ? child.path().filename().string()
                        : name + "/" + child.path().filename().string());
        }
    };
    append(root, ".");
    const auto closed = archive_write_close(archive.get());
    output.rethrow();
    checked(archive.get(), closed);
}

} // namespace ilemu
