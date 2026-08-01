#include "ilemu/macho.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <fstream>
#include <limits>
#include <set>
#include <span>
#include <sstream>
#include <stdexcept>

namespace ilemu {
namespace {

constexpr std::uint32_t mh_magic = 0xfeedfaceU;
constexpr std::uint32_t cpu_type_arm = 12;
constexpr std::uint32_t lc_segment = 0x1;
constexpr std::uint32_t lc_symtab = 0x2;
constexpr std::uint32_t lc_dysymtab = 0xb;
constexpr std::uint32_t lc_thread = 0x4;
constexpr std::uint32_t lc_unixthread = 0x5;
constexpr std::uint32_t lc_load_dylib = 0xc;
constexpr std::uint32_t lc_id_dylib = 0xd;
constexpr std::uint32_t lc_load_dylinker = 0xe;
constexpr std::uint32_t lc_id_dylinker = 0xf;
constexpr std::uint32_t lc_prebound_dylib = 0x10;
constexpr std::uint32_t lc_load_weak_dylib = 0x80000018U;
constexpr std::uint32_t lc_reexport_dylib = 0x8000001fU;
constexpr std::uint32_t lc_lazy_load_dylib = 0x20;
constexpr std::uint32_t lc_load_upward_dylib = 0x80000023U;
constexpr std::uint32_t arm_thread_state = 1;
constexpr std::uint32_t section_type_mask = 0xff;
constexpr std::uint32_t s_symbol_stubs = 0x8;
constexpr std::uint32_t indirect_symbol_local = 0x80000000U;
constexpr std::uint32_t indirect_symbol_abs = 0x40000000U;

std::uint32_t read_u32(std::span<const std::byte> bytes, std::size_t offset) {
    if (offset > bytes.size() || bytes.size() - offset < 4) {
        throw std::runtime_error{"truncated Mach-O integer"};
    }
    return std::to_integer<std::uint32_t>(bytes[offset]) |
           (std::to_integer<std::uint32_t>(bytes[offset + 1]) << 8U) |
           (std::to_integer<std::uint32_t>(bytes[offset + 2]) << 16U) |
           (std::to_integer<std::uint32_t>(bytes[offset + 3]) << 24U);
}

std::int32_t read_i32(std::span<const std::byte> bytes, std::size_t offset) {
    return static_cast<std::int32_t>(read_u32(bytes, offset));
}

std::optional<std::size_t> vm_file_offset(
    std::span<const MachSegment> segments, std::uint32_t address,
    std::size_t required_size) {
    for (const auto& segment : segments) {
        if (address < segment.vm_address ||
            address - segment.vm_address >= segment.file_size) {
            continue;
        }
        const auto delta = address - segment.vm_address;
        if (required_size > segment.file_size - delta) return std::nullopt;
        return static_cast<std::size_t>(segment.file_offset) + delta;
    }
    return std::nullopt;
}

std::optional<std::string_view> vm_c_string(
    std::span<const std::byte> bytes, std::span<const MachSegment> segments,
    std::uint32_t address) {
    const auto offset = vm_file_offset(segments, address, 1U);
    if (!offset || *offset >= bytes.size()) return std::nullopt;
    auto end = *offset;
    while (end < bytes.size() && bytes[end] != std::byte{0}) ++end;
    if (end == bytes.size()) return std::nullopt;
    return std::string_view{
        reinterpret_cast<const char*>(bytes.data() + *offset), end - *offset};
}

std::string fixed_string(std::span<const std::byte> bytes, std::size_t offset, std::size_t length) {
    if (offset > bytes.size() || bytes.size() - offset < length) {
        throw std::runtime_error{"truncated Mach-O string"};
    }
    std::size_t actual = 0;
    while (actual < length && bytes[offset + actual] != std::byte{0}) {
        ++actual;
    }
    return std::string{reinterpret_cast<const char*>(bytes.data() + offset), actual};
}

std::string command_string(
    std::span<const std::byte> bytes,
    std::size_t command_offset,
    std::uint32_t command_size,
    std::uint32_t string_offset) {
    if (string_offset >= command_size) {
        throw std::runtime_error{"invalid Mach-O load-command string offset"};
    }
    const auto start = command_offset + string_offset;
    const auto end = command_offset + command_size;
    std::size_t cursor = start;
    while (cursor < end && bytes[cursor] != std::byte{0}) {
        ++cursor;
    }
    if (cursor == end) {
        throw std::runtime_error{"unterminated Mach-O load-command string"};
    }
    return std::string{reinterpret_cast<const char*>(bytes.data() + start), cursor - start};
}

std::string table_string(
    std::span<const std::byte> bytes,
    std::uint32_t table_offset,
    std::uint32_t table_size,
    std::uint32_t string_offset) {
    if (string_offset >= table_size || table_offset > bytes.size() ||
        table_size > bytes.size() - table_offset) {
        throw std::runtime_error{"invalid Mach-O string-table offset"};
    }
    const auto start = static_cast<std::size_t>(table_offset) + string_offset;
    const auto end = static_cast<std::size_t>(table_offset) + table_size;
    std::size_t cursor = start;
    while (cursor < end && bytes[cursor] != std::byte{0}) {
        ++cursor;
    }
    if (cursor == end) {
        throw std::runtime_error{"unterminated Mach-O symbol name"};
    }
    return std::string{reinterpret_cast<const char*>(bytes.data() + start), cursor - start};
}

bool is_dylib_command(std::uint32_t command) {
    return command == lc_load_dylib || command == lc_id_dylib ||
           command == lc_load_weak_dylib || command == lc_reexport_dylib ||
           command == lc_lazy_load_dylib || command == lc_load_upward_dylib;
}

MemoryPermission vm_protection(std::int32_t protection) {
    MemoryPermission result = MemoryPermission::None;
    if ((protection & 1) != 0) result |= MemoryPermission::Read;
    if ((protection & 2) != 0) result |= MemoryPermission::Write;
    if ((protection & 4) != 0) result |= MemoryPermission::Execute;
    return result;
}

std::uint32_t align_down(std::uint32_t value) {
    return value & ~(AddressSpace::page_size - 1U);
}

std::uint32_t align_up(std::uint64_t value) {
    const auto aligned = (value + AddressSpace::page_size - 1U) &
                         ~(static_cast<std::uint64_t>(AddressSpace::page_size) - 1U);
    if (aligned > std::numeric_limits<std::uint32_t>::max()) {
        throw std::runtime_error{"Mach-O mapping exceeds 32-bit address space"};
    }
    return static_cast<std::uint32_t>(aligned);
}

}  // namespace

MachOImage MachOImage::parse(const std::filesystem::path& path) {
    std::ifstream input{path, std::ios::binary | std::ios::ate};
    if (!input) {
        throw std::runtime_error{"cannot open Mach-O: " + path.string()};
    }
    const auto end = input.tellg();
    if (end < 0) {
        throw std::runtime_error{"cannot determine Mach-O size: " + path.string()};
    }
    MachOImage image;
    image.path_ = path;
    image.bytes_.resize(static_cast<std::size_t>(end));
    input.seekg(0);
    if (!image.bytes_.empty()) {
        input.read(reinterpret_cast<char*>(image.bytes_.data()),
                   static_cast<std::streamsize>(image.bytes_.size()));
    }
    if (!input && !image.bytes_.empty()) {
        throw std::runtime_error{"failed to read Mach-O: " + path.string()};
    }

    const std::span<const std::byte> bytes{image.bytes_};
    if (bytes.size() < 28 || read_u32(bytes, 0) != mh_magic) {
        throw std::runtime_error{"expected a little-endian 32-bit Mach-O: " + path.string()};
    }
    image.cpu_type_ = read_u32(bytes, 4);
    image.cpu_subtype_ = read_u32(bytes, 8);
    image.file_type_ = read_u32(bytes, 12);
    image.command_count_ = read_u32(bytes, 16);
    const auto command_bytes = read_u32(bytes, 20);
    image.flags_ = read_u32(bytes, 24);
    if (image.cpu_type_ != cpu_type_arm) {
        throw std::runtime_error{"only 32-bit ARM Mach-O images are supported"};
    }
    if (command_bytes > bytes.size() - 28) {
        throw std::runtime_error{"Mach-O load command area is truncated"};
    }

    std::size_t offset = 28;
    const auto commands_end = offset + command_bytes;
    std::optional<std::pair<std::uint32_t, std::uint32_t>> indirect_symbols;
    std::set<std::uint32_t> known_generic{
        0x2, 0x3, 0x6, 0x7, 0x8, 0x9, 0xa, 0xb, 0x11, 0x12, 0x13, 0x14,
        0x15, 0x16, 0x17, 0x19, 0x1a, 0x1b, 0x1d, 0x1e, 0x21, 0x80000022U,
        0x24, 0x25, 0x26, 0x29, 0x2a, 0x2b, 0x2c, 0x2d, 0x2e, 0x2f,
        0x30, 0x31, 0x32, 0x80000033U, 0x34, 0x35,
    };

    for (std::uint32_t index = 0; index < image.command_count_; ++index) {
        if (offset > commands_end || commands_end - offset < 8) {
            throw std::runtime_error{"truncated Mach-O load command header"};
        }
        const auto command = read_u32(bytes, offset);
        const auto command_size = read_u32(bytes, offset + 4);
        if (command_size < 8 || command_size > commands_end - offset) {
            throw std::runtime_error{"invalid Mach-O load command size"};
        }

        if (command == lc_segment) {
            if (command_size < 56) {
                throw std::runtime_error{"truncated LC_SEGMENT"};
            }
            MachSegment segment;
            segment.name = fixed_string(bytes, offset + 8, 16);
            segment.vm_address = read_u32(bytes, offset + 24);
            segment.vm_size = read_u32(bytes, offset + 28);
            segment.file_offset = read_u32(bytes, offset + 32);
            segment.file_size = read_u32(bytes, offset + 36);
            segment.max_protection = read_i32(bytes, offset + 40);
            segment.initial_protection = read_i32(bytes, offset + 44);
            const auto section_count = read_u32(bytes, offset + 48);
            segment.flags = read_u32(bytes, offset + 52);
            if (section_count > (command_size - 56) / 68) {
                throw std::runtime_error{"LC_SEGMENT section array is truncated"};
            }
            for (std::uint32_t section_index = 0; section_index < section_count; ++section_index) {
                const auto section_offset = offset + 56 + section_index * 68;
                MachSection section;
                section.name = fixed_string(bytes, section_offset, 16);
                section.segment = fixed_string(bytes, section_offset + 16, 16);
                section.address = read_u32(bytes, section_offset + 32);
                section.size = read_u32(bytes, section_offset + 36);
                section.file_offset = read_u32(bytes, section_offset + 40);
                section.flags = read_u32(bytes, section_offset + 56);
                section.reserved1 = read_u32(bytes, section_offset + 60);
                section.reserved2 = read_u32(bytes, section_offset + 64);
                segment.sections.push_back(std::move(section));
            }
            image.segments_.push_back(std::move(segment));
        } else if (command == lc_symtab) {
            if (command_size < 24) {
                throw std::runtime_error{"truncated LC_SYMTAB"};
            }
            const auto symbol_offset = read_u32(bytes, offset + 8);
            const auto symbol_count = read_u32(bytes, offset + 12);
            const auto string_offset = read_u32(bytes, offset + 16);
            const auto string_size = read_u32(bytes, offset + 20);
            const auto symbol_bytes = static_cast<std::uint64_t>(symbol_count) * 12U;
            if (symbol_offset > bytes.size() || symbol_bytes > bytes.size() - symbol_offset ||
                string_offset > bytes.size() || string_size > bytes.size() - string_offset) {
                throw std::runtime_error{"LC_SYMTAB points outside the Mach-O"};
            }
            image.symbols_.reserve(symbol_count);
            for (std::uint32_t symbol_index = 0; symbol_index < symbol_count; ++symbol_index) {
                const auto symbol = static_cast<std::size_t>(symbol_offset) + symbol_index * 12U;
                const auto name_offset = read_u32(bytes, symbol);
                MachSymbol result;
                if (name_offset != 0) {
                    result.name = table_string(bytes, string_offset, string_size, name_offset);
                }
                result.type = std::to_integer<std::uint8_t>(bytes[symbol + 4]);
                result.section = std::to_integer<std::uint8_t>(bytes[symbol + 5]);
                result.description = static_cast<std::uint16_t>(
                    std::to_integer<std::uint16_t>(bytes[symbol + 6]) |
                    (std::to_integer<std::uint16_t>(bytes[symbol + 7]) << 8U));
                result.value = read_u32(bytes, symbol + 8);
                image.symbols_.push_back(std::move(result));
            }
        } else if (command == lc_dysymtab) {
            if (command_size < 80) {
                throw std::runtime_error{"truncated LC_DYSYMTAB"};
            }
            indirect_symbols = std::pair{read_u32(bytes, offset + 56),
                                         read_u32(bytes, offset + 60)};
        } else if (command == lc_thread || command == lc_unixthread) {
            std::size_t cursor = offset + 8;
            const auto end_offset = offset + command_size;
            while (cursor + 8 <= end_offset) {
                const auto flavor = read_u32(bytes, cursor);
                const auto count = read_u32(bytes, cursor + 4);
                cursor += 8;
                const auto state_bytes = static_cast<std::uint64_t>(count) * 4U;
                if (state_bytes > end_offset - cursor) {
                    throw std::runtime_error{"truncated LC_UNIXTHREAD state"};
                }
                if (flavor == arm_thread_state && count >= 17) {
                    image.entry_point_ = read_u32(bytes, cursor + 15 * 4U);
                }
                cursor += static_cast<std::size_t>(state_bytes);
            }
        } else if (is_dylib_command(command)) {
            if (command_size < 24) {
                throw std::runtime_error{"truncated dylib load command"};
            }
            image.dylibs_.push_back(
                MachDylib{command_string(bytes, offset, command_size, read_u32(bytes, offset + 8)),
                          command,
                          false});
        } else if (command == lc_prebound_dylib) {
            if (command_size < 20) {
                throw std::runtime_error{"truncated LC_PREBOUND_DYLIB"};
            }
            image.dylibs_.push_back(
                MachDylib{command_string(bytes, offset, command_size, read_u32(bytes, offset + 8)),
                          command,
                          true});
        } else if (command == lc_load_dylinker || command == lc_id_dylinker) {
            if (command_size < 12) {
                throw std::runtime_error{"truncated dylinker load command"};
            }
            image.dynamic_linker_ =
                command_string(bytes, offset, command_size, read_u32(bytes, offset + 8));
        } else if (!known_generic.contains(command)) {
            image.unknown_commands_.push_back(command);
        }
        offset += command_size;
    }
    if (offset != commands_end) {
        throw std::runtime_error{"Mach-O load command sizes do not match sizeofcmds"};
    }
    if (indirect_symbols) {
        const auto [table_offset, table_count] = *indirect_symbols;
        const auto table_bytes = static_cast<std::uint64_t>(table_count) * 4U;
        if (table_offset > bytes.size() || table_bytes > bytes.size() - table_offset) {
            throw std::runtime_error{"LC_DYSYMTAB indirect table points outside the Mach-O"};
        }
        for (const auto& segment : image.segments_) {
            for (const auto& section : segment.sections) {
                if ((section.flags & section_type_mask) != s_symbol_stubs ||
                    section.reserved2 == 0) {
                    continue;
                }
                const auto stub_count = section.size / section.reserved2;
                if (section.reserved1 > table_count ||
                    stub_count > table_count - section.reserved1) {
                    throw std::runtime_error{"symbol stub section exceeds indirect symbol table"};
                }
                for (std::uint32_t stub_index = 0; stub_index < stub_count; ++stub_index) {
                    const auto indirect_index = read_u32(
                        bytes, table_offset + (section.reserved1 + stub_index) * 4U);
                    if ((indirect_index & (indirect_symbol_local | indirect_symbol_abs)) != 0 ||
                        indirect_index >= image.symbols_.size()) {
                        continue;
                    }
                    image.stubs_.push_back(MachStub{
                        section.address + stub_index * section.reserved2,
                        section.reserved2,
                        image.symbols_[indirect_index].name});
                }
            }
        }
    }
    return image;
}

const MachSymbol* MachOImage::find_symbol(std::string_view name) const {
    const auto it = std::find_if(symbols_.begin(), symbols_.end(),
                                 [name](const MachSymbol& symbol) { return symbol.name == name; });
    return it == symbols_.end() ? nullptr : &*it;
}

const MachStub* MachOImage::find_stub(std::uint32_t address) const {
    const auto it = std::find_if(stubs_.begin(), stubs_.end(), [address](const MachStub& stub) {
        return address >= stub.address && address - stub.address < stub.size;
    });
    return it == stubs_.end() ? nullptr : &*it;
}

std::optional<std::uint32_t> MachOImage::read_vm_u32(std::uint32_t address) const {
    for (const auto& segment : segments_) {
        if (address < segment.vm_address || address - segment.vm_address > segment.file_size ||
            segment.file_size - (address - segment.vm_address) < 4) {
            continue;
        }
        const auto offset = static_cast<std::size_t>(segment.file_offset) +
                            (address - segment.vm_address);
        return read_u32(bytes_, offset);
    }
    return std::nullopt;
}

std::optional<std::uint16_t> MachOImage::read_vm_u16(std::uint32_t address) const {
    for (const auto& segment : segments_) {
        if (address < segment.vm_address || address - segment.vm_address > segment.file_size ||
            segment.file_size - (address - segment.vm_address) < 2) {
            continue;
        }
        const auto offset = static_cast<std::size_t>(segment.file_offset) +
                            (address - segment.vm_address);
        const std::span<const std::byte> bytes{bytes_};
        return static_cast<std::uint16_t>(
            std::to_integer<std::uint16_t>(bytes[offset]) |
            (std::to_integer<std::uint16_t>(bytes[offset + 1]) << 8U));
    }
    return std::nullopt;
}

std::optional<std::uint32_t> MachOImage::find_objc_instance_method(
    std::string_view class_name, std::string_view selector) const {
    constexpr std::uint32_t objc1_class_name_offset = 8U;
    constexpr std::uint32_t objc1_class_method_lists_offset = 28U;
    constexpr std::uint32_t objc2_class_data_offset = 16U;
    constexpr std::uint32_t objc2_class_ro_name_offset = 16U;
    constexpr std::uint32_t objc2_class_ro_methods_offset = 20U;
    constexpr std::uint32_t objc2_data_pointer_flags = 0x3U;
    constexpr std::uint32_t method_list_header_size = 8U;
    constexpr std::uint32_t method_size = 12U;
    constexpr std::uint32_t method_entry_size_mask = 0x0000fffcU;
    constexpr std::uint32_t maximum_method_lists = 4096U;
    constexpr std::uint32_t maximum_methods_per_list = 4096U;

    const auto executable = [&](std::uint32_t implementation) {
        const auto address = implementation & ~1U;
        return std::any_of(
            segments_.begin(), segments_.end(), [&](const MachSegment& segment) {
                return (segment.initial_protection & 4) != 0 &&
                       address >= segment.vm_address &&
                       address - segment.vm_address < segment.file_size;
            });
    };
    const auto search_list = [&](std::uint32_t list,
                                 std::uint32_t entry_size)
        -> std::optional<std::uint32_t> {
        if (entry_size < method_size ||
            entry_size > maximum_methods_per_list) {
            return std::nullopt;
        }
        if (list > std::numeric_limits<std::uint32_t>::max() - 4U) {
            return std::nullopt;
        }
        const auto count = read_vm_u32(list + 4U);
        if (!count || *count > maximum_methods_per_list) return std::nullopt;
        for (std::uint32_t index = 0; index < *count; ++index) {
            const auto entry64 = static_cast<std::uint64_t>(list) +
                                 method_list_header_size +
                                 static_cast<std::uint64_t>(index) * entry_size;
            if (entry64 >
                std::numeric_limits<std::uint32_t>::max() - 8U) {
                return std::nullopt;
            }
            const auto entry = static_cast<std::uint32_t>(entry64);
            const auto name = read_vm_u32(entry);
            const auto implementation = read_vm_u32(entry + 8U);
            if (!name || !implementation) return std::nullopt;
            const auto candidate = vm_c_string(bytes_, segments_, *name);
            if (candidate && *candidate == selector &&
                executable(*implementation)) {
                return implementation;
            }
        }
        return std::nullopt;
    };

    // Objective-C 2 stores pointers to class_t records in a dedicated
    // class-list section. Follow class_t -> class_ro_t -> method_list_t and
    // honor the entry stride published by the runtime metadata.
    for (const auto& segment : segments_) {
        for (const auto& section : segment.sections) {
            if (section.name != "__objc_classlist") continue;
            for (std::uint64_t section_offset = 0;
                 section_offset + sizeof(std::uint32_t) <= section.size;
                 section_offset += sizeof(std::uint32_t)) {
                const auto pointer64 =
                    static_cast<std::uint64_t>(section.address) +
                    section_offset;
                if (pointer64 > std::numeric_limits<std::uint32_t>::max()) {
                    break;
                }
                const auto object = read_vm_u32(
                    static_cast<std::uint32_t>(pointer64));
                if (!object || *object == 0U ||
                    *object > std::numeric_limits<std::uint32_t>::max() -
                                  objc2_class_data_offset) {
                    continue;
                }
                const auto data =
                    read_vm_u32(*object + objc2_class_data_offset);
                if (!data) continue;
                const auto class_ro = *data & ~objc2_data_pointer_flags;
                if (class_ro == 0U ||
                    class_ro > std::numeric_limits<std::uint32_t>::max() -
                                   objc2_class_ro_methods_offset) {
                    continue;
                }
                const auto name =
                    read_vm_u32(class_ro + objc2_class_ro_name_offset);
                if (!name) continue;
                const auto candidate =
                    vm_c_string(bytes_, segments_, *name);
                if (!candidate || *candidate != class_name) continue;
                const auto methods =
                    read_vm_u32(class_ro + objc2_class_ro_methods_offset);
                if (!methods || *methods == 0U) return std::nullopt;
                const auto entsize_and_flags = read_vm_u32(*methods);
                if (!entsize_and_flags) return std::nullopt;
                const auto entry_size =
                    *entsize_and_flags & method_entry_size_mask;
                return search_list(*methods, entry_size);
            }
        }
    }

    for (const auto& segment : segments_) {
        for (const auto& section : segment.sections) {
            if (section.segment != "__OBJC" || section.name != "__class") {
                continue;
            }
            // Objective-C 1 class records retained the offsets used below,
            // but their trailing runtime fields make the total record stride
            // vary between old toolchains. Scan aligned record candidates so
            // semantic lookup does not bake in either the 40- or 48-byte form.
            for (std::uint64_t section_offset = 0;
                 section_offset + objc1_class_method_lists_offset +
                         sizeof(std::uint32_t) <=
                     section.size;
                 section_offset += alignof(std::uint32_t)) {
                const auto object64 =
                    static_cast<std::uint64_t>(section.address) +
                    section_offset;
                if (object64 > std::numeric_limits<std::uint32_t>::max()) {
                    break;
                }
                const auto object = static_cast<std::uint32_t>(object64);
                const auto name =
                    read_vm_u32(object + objc1_class_name_offset);
                if (!name) continue;
                const auto candidate = vm_c_string(bytes_, segments_, *name);
                if (!candidate || *candidate != class_name) continue;
                const auto lists =
                    read_vm_u32(object + objc1_class_method_lists_offset);
                if (!lists || *lists == 0U || *lists == 0xffffffffU) {
                    continue;
                }
                // The common Objective-C 1.x form points directly at one
                // method list. Some images instead use a null-terminated array
                // of list pointers; accept both layouts.
                if (const auto direct = search_list(*lists, method_size))
                    return direct;
                for (std::uint32_t list_index = 0;
                     list_index < maximum_method_lists; ++list_index) {
                    const auto pointer_address =
                        static_cast<std::uint64_t>(*lists) +
                        static_cast<std::uint64_t>(list_index) *
                            sizeof(std::uint32_t);
                    if (pointer_address >
                        std::numeric_limits<std::uint32_t>::max()) {
                        break;
                    }
                    const auto pointer = read_vm_u32(
                        static_cast<std::uint32_t>(pointer_address));
                    if (!pointer || *pointer == 0U || *pointer == 0xffffffffU) {
                        break;
                    }
                    if (const auto found =
                            search_list(*pointer, method_size)) {
                        return found;
                    }
                }
                continue;
            }
        }
    }
    return std::nullopt;
}

void MachOImage::map_into(AddressSpace& memory) const {
    const std::span<const std::byte> bytes{bytes_};
    for (const auto& segment : segments_) {
        if (segment.vm_size == 0 || segment.name == "__PAGEZERO") {
            continue;
        }
        if (segment.file_size > segment.vm_size ||
            segment.file_offset > bytes.size() ||
            segment.file_size > bytes.size() - segment.file_offset) {
            throw std::runtime_error{"invalid file range in segment " + segment.name};
        }
        const auto base = align_down(segment.vm_address);
        const auto prefix = segment.vm_address - base;
        const auto mapping_size = align_up(static_cast<std::uint64_t>(prefix) + segment.vm_size);
        if (!memory.map(base, mapping_size, vm_protection(segment.initial_protection))) {
            throw std::runtime_error{"failed to map segment " + segment.name};
        }
        if (segment.file_size != 0 &&
            !memory.copy_in(segment.vm_address,
                            bytes.subspan(segment.file_offset, segment.file_size))) {
            throw std::runtime_error{"failed to populate segment " + segment.name};
        }
    }
}

std::string mach_file_type_name(std::uint32_t file_type) {
    switch (file_type) {
    case 1: return "object";
    case 2: return "executable";
    case 3: return "fixed-vm shared library";
    case 4: return "core";
    case 5: return "preloaded executable";
    case 6: return "dynamic library";
    case 7: return "dynamic linker";
    case 8: return "bundle";
    default: return "type-" + std::to_string(file_type);
    }
}

std::string mach_cpu_name(std::uint32_t cpu_type, std::uint32_t cpu_subtype) {
    if (cpu_type != cpu_type_arm) {
        return "cpu-" + std::to_string(cpu_type);
    }
    switch (cpu_subtype & 0xffU) {
    case 5: return "ARMv4T";
    case 6: return "ARMv6";
    case 7: return "ARMv5TEJ";
    case 8: return "ARM-XScale";
    case 9: return "ARMv7";
    default: return "ARM-subtype-" + std::to_string(cpu_subtype & 0xffU);
    }
}

}  // namespace ilemu
