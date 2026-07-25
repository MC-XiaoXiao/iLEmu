#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "ilemu/address_space.hpp"

namespace ilemu {

struct MachSection {
    std::string name;
    std::string segment;
    std::uint32_t address{};
    std::uint32_t size{};
    std::uint32_t file_offset{};
    std::uint32_t flags{};
    std::uint32_t reserved1{};
    std::uint32_t reserved2{};
};

struct MachStub {
    std::uint32_t address{};
    std::uint32_t size{};
    std::string symbol;
};

struct MachSegment {
    std::string name;
    std::uint32_t vm_address{};
    std::uint32_t vm_size{};
    std::uint32_t file_offset{};
    std::uint32_t file_size{};
    std::int32_t max_protection{};
    std::int32_t initial_protection{};
    std::uint32_t flags{};
    std::vector<MachSection> sections;
};

struct MachDylib {
    std::string path;
    std::uint32_t command{};
    bool prebound{};
};

struct MachSymbol {
    std::string name;
    std::uint32_t value{};
    std::uint8_t type{};
    std::uint8_t section{};
    std::uint16_t description{};

    [[nodiscard]] bool thumb_definition() const {
        // Mach-O ARM N_ARM_THUMB_DEF in nlist::n_desc.
        return (description & 0x0008U) != 0;
    }
};

class MachOImage {
public:
    static MachOImage parse(const std::filesystem::path& path);

    [[nodiscard]] const std::filesystem::path& path() const { return path_; }
    [[nodiscard]] std::uint32_t cpu_type() const { return cpu_type_; }
    [[nodiscard]] std::uint32_t cpu_subtype() const { return cpu_subtype_; }
    [[nodiscard]] std::uint32_t file_type() const { return file_type_; }
    [[nodiscard]] std::uint32_t flags() const { return flags_; }
    [[nodiscard]] std::uint32_t command_count() const { return command_count_; }
    [[nodiscard]] const std::vector<MachSegment>& segments() const { return segments_; }
    [[nodiscard]] const std::vector<MachDylib>& dylibs() const { return dylibs_; }
    [[nodiscard]] const std::vector<MachSymbol>& symbols() const { return symbols_; }
    [[nodiscard]] const std::vector<MachStub>& stubs() const { return stubs_; }
    [[nodiscard]] const std::optional<std::string>& dynamic_linker() const { return dynamic_linker_; }
    [[nodiscard]] const std::optional<std::uint32_t>& entry_point() const { return entry_point_; }
    [[nodiscard]] const std::vector<std::uint32_t>& unknown_commands() const { return unknown_commands_; }

    [[nodiscard]] const MachSymbol* find_symbol(std::string_view name) const;
    [[nodiscard]] const MachStub* find_stub(std::uint32_t address) const;
    [[nodiscard]] std::optional<std::uint16_t> read_vm_u16(std::uint32_t address) const;
    [[nodiscard]] std::optional<std::uint32_t> read_vm_u32(std::uint32_t address) const;
    // Resolve an Objective-C 1.x method implementation from the image's
    // metadata. This covers stripped firmware methods without relying on a
    // version-specific virtual address.
    [[nodiscard]] std::optional<std::uint32_t> find_objc_instance_method(
        std::string_view class_name, std::string_view selector) const;

    void map_into(AddressSpace& memory) const;

private:
    std::filesystem::path path_;
    std::vector<std::byte> bytes_;
    std::uint32_t cpu_type_{};
    std::uint32_t cpu_subtype_{};
    std::uint32_t file_type_{};
    std::uint32_t flags_{};
    std::uint32_t command_count_{};
    std::vector<MachSegment> segments_;
    std::vector<MachDylib> dylibs_;
    std::vector<MachSymbol> symbols_;
    std::vector<MachStub> stubs_;
    std::optional<std::string> dynamic_linker_;
    std::optional<std::uint32_t> entry_point_;
    std::vector<std::uint32_t> unknown_commands_;
};

[[nodiscard]] std::string mach_file_type_name(std::uint32_t file_type);
[[nodiscard]] std::string mach_cpu_name(std::uint32_t cpu_type, std::uint32_t cpu_subtype);

}  // namespace ilemu
