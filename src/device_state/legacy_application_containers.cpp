/* SPDX-License-Identifier: MPL-2.0 */
#include "device_state/legacy_application_containers.hpp"

#if defined(ILEMU_HAS_LIBPLIST)
#include "foundation/rootfs_path_resolver.hpp"

#include <cerrno>
#include <cstdlib>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include <dirent.h>
#include <fcntl.h>
#include <plist/plist.h>
#include <sys/stat.h>
#include <sys/xattr.h>
#include <unistd.h>
#endif

namespace ilemu {
#if defined(ILEMU_HAS_LIBPLIST)
namespace {
    constexpr char installation_cache[] = "com.apple.mobile.installation.plist";
    constexpr char invalidation_pending[] =
        ".ilemu-launch-services-invalidation";
    constexpr std::string_view applications_prefix {
        "/var/mobile/Applications/"
    };
    constexpr std::string_view private_prefix { "/private" };
    constexpr int directory_flags =
        O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC;

    struct Descriptor {
        explicit Descriptor(int fd)
            : value { fd }
        {
        }
        ~Descriptor()
        {
            if (value >= 0)
                ::close(value);
        }
        Descriptor(const Descriptor&) = delete;
        Descriptor& operator=(const Descriptor&) = delete;
        const int value;
    };

    [[noreturn]] void fail(const char* operation)
    {
        throw std::system_error(errno, std::generic_category(), operation);
    }

    std::string_view plist_string(plist_t node)
    {
        std::uint64_t size { };
        const auto* value = plist_get_string_ptr(node, &size);
        return value
                   ? std::string_view { value, static_cast<std::size_t>(size) }
                   : std::string_view { };
    }

    std::string_view without_private_prefix(std::string_view path)
    {
        if (path.starts_with("/private/"))
            path.remove_prefix(private_prefix.size());
        return path;
    }

    bool is_container_uuid(std::string_view value)
    {
        if (value.size() != 36U)
            return false;
        for (std::size_t index = 0; index < value.size(); ++index) {
            const auto character = value[index];
            if (index == 8U || index == 13U || index == 18U || index == 23U) {
                if (character != '-')
                    return false;
            } else if (std::string_view { "0123456789abcdefABCDEF" }.find(
                           character) == std::string_view::npos) {
                return false;
            }
        }
        return true;
    }

    bool ensure_mobile_directory(int parent, const char* name)
    {
        if (::mkdirat(parent, name, 0700) != 0) {
            if (errno != EEXIST)
                fail("create legacy app data directory");
            const Descriptor directory { ::openat(
                parent, name, directory_flags) };
            return directory.value >= 0;
        }
        const Descriptor directory { ::openat(parent, name, directory_flags) };
        if (directory.value < 0)
            return false;
        // Host permissions belong to the emulator's account. HFS metadata
        // separately records the guest's mobile user and group.
        constexpr char mobile[] = "501";
        if (::fsetxattr(directory.value, "user.hfsfuse.record.owner_id", mobile,
                sizeof(mobile), 0) != 0 ||
            ::fsetxattr(directory.value, "user.hfsfuse.record.group_id", mobile,
                sizeof(mobile), 0) != 0) {
            const auto error = errno;
            ::unlinkat(parent, name, AT_REMOVEDIR);
            errno = error;
            fail("record legacy app data directory ownership");
        }
        return true;
    }

    bool directory_or_missing(int parent, const char* name)
    {
        struct stat mode { };
        if (::fstatat(parent, name, &mode, AT_SYMLINK_NOFOLLOW) == 0)
            return S_ISDIR(mode.st_mode);
        return errno == ENOENT;
    }

    bool prepare_record(int applications, plist_t record)
    {
        if (plist_get_node_type(record) != PLIST_DICT ||
            plist_string(plist_dict_get_item(record, "ApplicationType")) !=
                "User") {
            return false;
        }
        const auto path = plist_string(plist_dict_get_item(record, "Path"));
        auto relative = without_private_prefix(path);
        if (!relative.starts_with(applications_prefix))
            return false;
        relative.remove_prefix(applications_prefix.size());
        const auto separator = relative.find('/');
        if (separator == std::string_view::npos)
            return false;
        const auto uuid = relative.substr(0, separator);
        const auto bundle_name = relative.substr(separator + 1U);
        if (!is_container_uuid(uuid) || bundle_name.size() <= 4U ||
            !bundle_name.ends_with(".app") ||
            bundle_name.find('/') != std::string_view::npos ||
            bundle_name.find('\0') != std::string_view::npos) {
            return false;
        }
        const auto expected_container = path.substr(0, path.rfind('/'));
        const auto container = plist_dict_get_item(record, "Container");
        const auto existing_home = plist_string(container);
        if (container && without_private_prefix(existing_home) !=
                             without_private_prefix(expected_container)) {
            return false;
        }
        auto environment = plist_dict_get_item(record, "EnvironmentVariables");
        if (environment && plist_get_node_type(environment) != PLIST_DICT)
            return false;

        const Descriptor data { ::openat(
            applications, std::string { uuid }.c_str(), directory_flags) };
        const Descriptor bundle { ::openat(
            data.value, std::string { bundle_name }.c_str(), directory_flags) };
        struct stat manifest { };
        if (bundle.value < 0 ||
            ::fstatat(bundle.value, "Info.plist", &manifest,
                AT_SYMLINK_NOFOLLOW) != 0 ||
            !S_ISREG(manifest.st_mode)) {
            return false;
        }
        const Descriptor existing_library { ::openat(
            data.value, "Library", directory_flags) };
        if ((existing_library.value < 0 && errno != ENOENT) ||
            (existing_library.value >= 0 &&
                (!directory_or_missing(existing_library.value, "Caches") ||
                    !directory_or_missing(
                        existing_library.value, "Preferences"))) ||
            !directory_or_missing(data.value, "Documents") ||
            !directory_or_missing(data.value, "tmp")) {
            return false;
        }
        // Repair empty directory slots without replacing any guest content.
        if (!ensure_mobile_directory(data.value, "Documents") ||
            !ensure_mobile_directory(data.value, "Library") ||
            !ensure_mobile_directory(data.value, "tmp")) {
            return false;
        }
        const Descriptor library { ::openat(
            data.value, "Library", directory_flags) };
        if (library.value < 0 ||
            !ensure_mobile_directory(library.value, "Caches") ||
            !ensure_mobile_directory(library.value, "Preferences")) {
            return false;
        }

        const std::string home { container ? existing_home
                                           : expected_container };
        bool changed = false;
        if (!container) {
            plist_dict_set_item(
                record, "Container", plist_new_string(home.c_str()));
            changed = true;
        }
        if (!environment) {
            environment = plist_new_dict();
            plist_dict_set_item(record, "EnvironmentVariables", environment);
            changed = true;
        }
        const auto add_default = [&](const char* key,
                                     const std::string& value) {
            if (plist_dict_get_item(environment, key))
                return;
            plist_dict_set_item(
                environment, key, plist_new_string(value.c_str()));
            changed = true;
        };
        add_default("HOME", home);
        add_default("CFFIXED_USER_HOME", home);
        add_default("TMPDIR", home + "/tmp/");
        return changed;
    }

    void copy_guest_attributes(int source, int destination)
    {
        const auto size = ::flistxattr(source, nullptr, 0);
        if (size < 0)
            fail("read MobileInstallation cache attributes");
        std::vector<char> names(static_cast<std::size_t>(size));
        if (::flistxattr(source, names.data(), names.size()) != size)
            fail("read MobileInstallation cache attributes");
        for (std::size_t index = 0; index < names.size();) {
            const auto* name = names.data() + index;
            const std::string_view key { name };
            index += key.size() + 1U;
            if (!key.starts_with("user."))
                continue;
            const auto length = ::fgetxattr(source, name, nullptr, 0);
            if (length < 0)
                fail("read MobileInstallation cache attribute");
            std::vector<char> value(static_cast<std::size_t>(length));
            if (::fgetxattr(source, name, value.data(), value.size()) !=
                    length ||
                ::fsetxattr(destination, name, value.data(), value.size(), 0) !=
                    0) {
                fail("preserve MobileInstallation cache attribute");
            }
        }
    }

    void mark_launch_services_invalidation(int caches)
    {
        const Descriptor marker { ::openat(caches, invalidation_pending,
            O_WRONLY | O_CREAT | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC, 0600) };
        struct stat info { };
        if (marker.value < 0 || ::fstat(marker.value, &info) != 0)
            fail("mark LaunchServices cache invalidation");
        if (!S_ISREG(info.st_mode))
            throw std::runtime_error { "invalid LaunchServices cache marker" };
        // Leave a durable marker before publishing the new installation map
        // so the next boot can finish an interrupted cache invalidation.
        if (::fsync(caches) != 0)
            fail("persist LaunchServices cache marker");
    }

    void invalidate_launch_services_caches(int caches)
    {
        const auto fd = ::openat(caches, ".", directory_flags);
        if (fd < 0)
            fail("open LaunchServices cache directory");
        const std::unique_ptr<DIR, decltype(&::closedir)> entries {
            ::fdopendir(fd), ::closedir
        };
        if (!entries) {
            const auto error = errno;
            ::close(fd);
            errno = error;
            fail("open LaunchServices cache directory stream");
        }
        for (;;) {
            errno = 0;
            const auto* entry = ::readdir(entries.get());
            if (!entry) {
                if (errno)
                    fail("read LaunchServices cache directory");
                break;
            }
            const std::string_view name { entry->d_name };
            if (!name.starts_with("com.apple.LaunchServices-") ||
                !name.ends_with(".csstore")) {
                continue;
            }
            struct stat info { };
            if (::fstatat(caches, entry->d_name, &info, AT_SYMLINK_NOFOLLOW) !=
                0) {
                if (errno == ENOENT)
                    continue;
                fail("inspect LaunchServices cache");
            }
            if (S_ISREG(info.st_mode) &&
                ::unlinkat(caches, entry->d_name, 0) != 0 && errno != ENOENT) {
                fail("invalidate LaunchServices cache");
            }
        }
        if (::fsync(caches) != 0)
            fail("persist LaunchServices cache invalidation");
        if (::unlinkat(caches, invalidation_pending, 0) != 0 && errno != ENOENT)
            fail("clear LaunchServices cache marker");
    }

    bool publish_cache(int caches, int input, const struct stat& original,
        plist_t root, plist_format_t format)
    {
        char* serialized = nullptr;
        std::uint32_t size { };
        const auto result = format == PLIST_FORMAT_BINARY
                                ? plist_to_bin(root, &serialized, &size)
                                : plist_to_xml(root, &serialized, &size);
        const std::unique_ptr<char, decltype(&std::free)> bytes { serialized,
            std::free };
        if (result != PLIST_ERR_SUCCESS || !bytes || size == 0U)
            throw std::runtime_error { "serialize MobileInstallation cache" };

        auto path = "/proc/self/fd/" + std::to_string(caches) +
                    "/.ilemu-installation-XXXXXX";
        const Descriptor output { ::mkstemp(path.data()) };
        if (output.value < 0)
            fail("stage MobileInstallation cache");
        struct TemporaryCache {
            int directory;
            const char* name;
            ~TemporaryCache() { ::unlinkat(directory, name, 0); }
        } temporary { caches, path.c_str() + path.rfind('/') + 1U };
        for (std::size_t offset = 0; offset < size;) {
            const auto written =
                ::write(output.value, bytes.get() + offset, size - offset);
            if (written < 0 && errno == EINTR)
                continue;
            if (written <= 0)
                fail("write MobileInstallation cache");
            offset += static_cast<std::size_t>(written);
        }
        copy_guest_attributes(input, output.value);
        if (::fchmod(output.value, original.st_mode & 0777U) != 0 ||
            ::fsync(output.value) != 0)
            fail("flush MobileInstallation cache");
        struct stat current { };
        if (::fstatat(caches, installation_cache, &current,
                AT_SYMLINK_NOFOLLOW) != 0 ||
            current.st_dev != original.st_dev ||
            current.st_ino != original.st_ino ||
            current.st_ctim.tv_sec != original.st_ctim.tv_sec ||
            current.st_ctim.tv_nsec != original.st_ctim.tv_nsec) {
            return false;
        }
        mark_launch_services_invalidation(caches);
        if (::renameat(caches, temporary.name, caches, installation_cache) != 0)
            fail("publish MobileInstallation cache");
        return true;
    }
} // namespace
#endif

void prepare_legacy_application_containers(
    const std::filesystem::path& rootfs, DarwinVersion version)
{
#if defined(ILEMU_HAS_LIBPLIST)
    if (version < DarwinGeneration::IphoneOs2 ||
        version >= DarwinGeneration::Ios8)
        return;
    const auto var_path =
        RootfsPathResolver { rootfs }.resolve("/var").lexically_relative(
            rootfs);
    if (var_path != "private/var" && var_path != "var")
        return;
    const Descriptor root { ::open(
        rootfs.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC) };
    const Descriptor private_dir { var_path == "private/var"
                                       ? ::openat(root.value, "private",
                                             directory_flags)
                                       : -1 };
    const auto var_parent =
        var_path == "private/var" ? private_dir.value : root.value;
    const Descriptor var { ::openat(var_parent, "var", directory_flags) };
    const Descriptor mobile { ::openat(var.value, "mobile", directory_flags) };
    const Descriptor applications { ::openat(
        mobile.value, "Applications", directory_flags) };
    const Descriptor library { ::openat(
        mobile.value, "Library", directory_flags) };
    const Descriptor caches { ::openat(
        library.value, "Caches", directory_flags) };
    if (applications.value < 0 || caches.value < 0)
        return;
    const Descriptor file { ::openat(caches.value, installation_cache,
        O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK) };
    struct stat info { };
    if (file.value < 0 || ::fstat(file.value, &info) != 0 ||
        !S_ISREG(info.st_mode) || info.st_size <= 0 ||
        static_cast<std::uintmax_t>(info.st_size) >
            std::numeric_limits<std::uint32_t>::max()) {
        return;
    }
    std::string bytes(static_cast<std::size_t>(info.st_size), '\0');
    for (std::size_t offset = 0; offset < bytes.size();) {
        const auto received =
            ::read(file.value, bytes.data() + offset, bytes.size() - offset);
        if (received < 0 && errno == EINTR)
            continue;
        if (received <= 0)
            return;
        offset += static_cast<std::size_t>(received);
    }
    plist_t parsed = nullptr;
    plist_format_t format = PLIST_FORMAT_NONE;
    const auto result = plist_from_memory(bytes.data(),
        static_cast<std::uint32_t>(bytes.size()), &parsed, &format);
    const std::unique_ptr<void, decltype(&plist_free)> document { parsed,
        plist_free };
    if (result != PLIST_ERR_SUCCESS || !document ||
        (format != PLIST_FORMAT_BINARY && format != PLIST_FORMAT_XML)) {
        return;
    }
    const auto user = plist_dict_get_item(parsed, "User");
    if (!user || plist_get_node_type(user) != PLIST_DICT)
        return;
    plist_dict_iter iterator = nullptr;
    plist_dict_new_iter(user, &iterator);
    const std::unique_ptr<void, decltype(&std::free)> entries { iterator,
        std::free };
    if (!entries)
        return;
    bool changed = false;
    for (;;) {
        plist_t record = nullptr;
        plist_dict_next_item(user, iterator, nullptr, &record);
        if (!record)
            break;
        changed = prepare_record(applications.value, record) || changed;
    }
    if (changed &&
        !publish_cache(caches.value, file.value, info, parsed, format))
        return;
    struct stat marker { };
    if (::fstatat(caches.value, invalidation_pending, &marker,
            AT_SYMLINK_NOFOLLOW) == 0 &&
        S_ISREG(marker.st_mode)) {
        invalidate_launch_services_caches(caches.value);
    }
#else
    static_cast<void>(rootfs);
    static_cast<void>(version);
#endif
}

} // namespace ilemu
