// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
#pragma once

#include <cstddef>
#include <filesystem>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace ilemu {

// Guest attributes travel with host inodes. Host access permissions are never
// guest permissions: every firmware directory must remain removable by its
// owner.
class FirmwareStorage {
public:
    using Attributes = std::map<std::string, std::vector<std::byte>>;
    static Attributes attributes(const std::filesystem::path& path);
    static void attributes(
        const std::filesystem::path& path, const Attributes& values);
    static void prepare(const std::filesystem::path& root);
    static void copy(const std::filesystem::path& source,
        const std::filesystem::path& destination);
    static void remove(const std::filesystem::path& root);
    static void normalize(
        const std::filesystem::path& path, Attributes values = { });
};

// libarchive supplies the archive codecs. Stream adapters belong to the
// frontend.
class FirmwareArchive {
public:
    using Reader = std::function<std::size_t(void*, std::size_t)>;
    using Writer = std::function<void(const void*, std::size_t)>;
    static void unpack(Reader read, const std::filesystem::path& root);
    static void pack(const std::filesystem::path& root, Writer write);
};

} // namespace ilemu
