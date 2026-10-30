// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
#include "app/firmware_command.hpp"
#include "filesystem/hfs_metadata.hpp"
#include "foundation/output.hpp"
#include "host/firmware_storage.hpp"
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <sys/stat.h>

namespace ilemu {
void firmware_storage_command(
    const std::vector<std::string>& arguments, Output& output)
{
    if (arguments.empty())
        throw std::runtime_error("firmware requires prepare, unpack, pack, "
                                 "normalize, copy, remove or stat");
    const auto option = [&](const std::string& name) {
        for (std::size_t index = 1; index < arguments.size(); ++index) {
            if (arguments[index] == name && index + 1 < arguments.size())
                return arguments[index + 1];
        }
        throw std::runtime_error("missing firmware option: " + name);
    };
    const auto root =
        std::filesystem::absolute(option("--rootfs")).lexically_normal();
    // Destructive maintenance commands must target a supplied firmware tree,
    // never a host filesystem root or the current working directory.
    if (root == root.root_path() || root == std::filesystem::current_path())
        throw std::runtime_error("invalid firmware maintenance root");
    const auto& command = arguments.front();
    if (command == "unpack") {
        if (std::filesystem::exists(std::filesystem::symlink_status(root)))
            throw std::runtime_error(
                "firmware unpack requires a new destination");
        std::ifstream file;
        std::istream* stream = &std::cin;
        const auto archive = option("--archive");
        if (archive != "-") {
            file.open(archive, std::ios::binary);
            if (!file)
                throw std::runtime_error("cannot open firmware archive");
            stream = &file;
        }
        try {
            FirmwareArchive::unpack(
                [&](void* bytes, std::size_t size) {
                    stream->read(static_cast<char*>(bytes),
                        static_cast<std::streamsize>(size));
                    if (stream->bad())
                        throw std::runtime_error(
                            "firmware archive read failed");
                    return static_cast<std::size_t>(stream->gcount());
                },
                root);
        } catch (...) {
            FirmwareStorage::remove(root);
            throw;
        }
    } else if (command == "pack") {
        const auto destination =
            std::filesystem::absolute(option("--archive")).lexically_normal();
        const auto relative = destination.lexically_relative(root);
        if (destination == root ||
            (!relative.empty() && *relative.begin() != ".."))
            throw std::runtime_error(
                "firmware archive must be outside the source tree");
        std::ofstream stream { destination,
            std::ios::binary | std::ios::trunc };
        if (!stream)
            throw std::runtime_error("cannot create firmware archive");
        FirmwareArchive::pack(root, [&](const void* bytes, std::size_t size) {
            if (!stream.write(static_cast<const char*>(bytes),
                    static_cast<std::streamsize>(size)))
                throw std::runtime_error("firmware archive write failed");
        });
        stream.flush();
        if (!stream)
            throw std::runtime_error("firmware archive flush failed");
    } else if (command == "normalize") {
        FirmwareStorage::prepare(root);
    } else if (command == "copy") {
        FirmwareStorage::copy(root, option("--destination"));
    } else if (command == "remove") {
        FirmwareStorage::remove(root);
    } else if (command == "stat") {
        struct stat host { };
        if (::lstat(root.c_str(), &host) != 0)
            throw std::runtime_error("cannot stat firmware path");
        const auto guest =
            hfs::MetadataProvider { root.root_path() }.query(root, false);
        if (!guest)
            throw std::runtime_error("cannot query guest firmware metadata");
        output.write("[firmware-stat] host-mode=" +
                     std::to_string(host.st_mode & 07777U) +
                     " host-owner=" + std::to_string(host.st_uid) +
                     " guest-mode=" + std::to_string(guest->mode & 07777U) +
                     " guest-owner=" + std::to_string(guest->owner) +
                     " guest-group=" + std::to_string(guest->group) +
                     " catalog-id=" + std::to_string(guest->catalog_id) + "\n");
    } else {
        throw std::runtime_error(
            "unknown firmware storage command: " + command);
    }
    output.write("[firmware] " + command + " complete\n");
}
}
