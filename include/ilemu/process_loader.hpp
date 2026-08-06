#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "ilemu/address_space.hpp"
#include "ilemu/macho.hpp"

namespace ilemu {

struct LoadedProcess {
    MachOImage executable;
    MachOImage dynamic_linker;
    std::string executable_path;
    std::vector<std::string> arguments;
    std::uint32_t entry_point{};
    std::uint32_t stack_pointer{};
    std::uint32_t main_header{};
};

class ProcessLoader {
public:
    ProcessLoader(
        std::filesystem::path rootfs, AddressSpace& memory,
        ArmArchitectureVersion architecture = ArmArchitectureVersion::Armv6K);

    LoadedProcess load(
        std::string guest_executable,
        std::vector<std::string> arguments = {},
        std::vector<std::string> environment = {});
    [[nodiscard]] bool validate(std::string guest_executable) const;

private:
    struct ResolvedInvocation {
        std::string executable_path;
        std::vector<std::string> arguments;
    };

    [[nodiscard]] std::filesystem::path host_path(const std::string& guest_path) const;
    [[nodiscard]] ResolvedInvocation resolve_invocation(
        std::string guest_executable,
        std::vector<std::string> arguments) const;

    std::filesystem::path rootfs_;
    AddressSpace& memory_;
    ArmArchitectureVersion architecture_;
};

}  // namespace ilemu
