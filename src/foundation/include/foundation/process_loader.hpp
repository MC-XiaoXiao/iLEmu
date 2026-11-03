// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// Map guest executables and libraries and construct their initial
// execution state.

#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "foundation/address_space.hpp"
#include "foundation/darwin_address_layout.hpp"
#include "foundation/darwin_version.hpp"
#include "foundation/macho.hpp"

namespace ilemu {

class ExecutableCatalog;

struct LoadedProcess {
    MachOImage executable;
    MachOImage dynamic_linker;
    std::string executable_path;
    std::vector<std::string> arguments;
    std::uint32_t entry_point { };
    std::uint32_t stack_pointer { };
    std::uint32_t main_header { };
};

// A frozen invocation and its immutable Mach-O snapshots. Preparation performs
// no guest-memory or CPU operations; mapping consumes this result exactly once.
struct PreparedProcessImage {
    std::string requested_path;
    std::string executable_path;
    std::vector<std::string> arguments;
    MachOImage executable;
    MachOImage dynamic_linker;
    // Retain admission until VM commit, including the runtime handoff.
    std::shared_ptr<void> preparation_admission;
};

class ProcessImageLoader {
public:
    ProcessImageLoader(std::filesystem::path rootfs,
        ArmArchitectureVersion architecture = ArmArchitectureVersion::Armv6K,
        ExecutableCatalog* catalog = nullptr);
    [[nodiscard]] PreparedProcessImage prepare(std::string guest_executable,
        std::vector<std::string> arguments = { }) const;

private:
    struct ResolvedInvocation {
        std::string executable_path;
        std::vector<std::string> arguments;
    };
    [[nodiscard]] std::filesystem::path host_path(
        const std::string& guest_path) const;
    [[nodiscard]] MachOImage parse_catalogued(
        const std::filesystem::path& path) const;
    [[nodiscard]] ResolvedInvocation resolve_invocation(
        std::string guest_executable, std::vector<std::string> arguments) const;
    std::filesystem::path rootfs_;
    ArmArchitectureVersion architecture_;
    ExecutableCatalog* catalog_ { };
};

class ProcessLoader {
public:
    ProcessLoader(std::filesystem::path rootfs, AddressSpace& memory,
        ArmArchitectureVersion architecture = ArmArchitectureVersion::Armv6K,
        ExecutableCatalog* catalog = nullptr, DarwinVersion version = { });

    LoadedProcess load(std::string guest_executable,
        std::vector<std::string> arguments = { },
        std::vector<std::string> environment = { });
    LoadedProcess load(PreparedProcessImage prepared,
        std::vector<std::string> environment = { });
    [[nodiscard]] bool validate(std::string guest_executable) const;

private:
    ProcessImageLoader images_;
    AddressSpace& memory_;
    ExecutableCatalog* catalog_ { };
    DarwinVersion version_;
};

} // namespace ilemu
