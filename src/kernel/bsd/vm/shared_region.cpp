#include "ilemu/kernel.hpp"

#include "../support.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <optional>
#include <string>
#include <system_error>
#include <vector>

namespace ilemu {
namespace {

// The iPhoneOS 1 ARM dyld uses five 32-bit words here.  This differs from
// xnu-792's desktop 32-bit ABI, whose mach_vm_* members make the entry 32
// bytes.  The layout below is confirmed by the firmware dyld's syscall 299
// call site and matches the ARM split-segment addresses in the system dylibs.
constexpr std::uint32_t arm_mapping_size = 5U * sizeof(std::uint32_t);
constexpr std::uint32_t mach_vm_mapping_size =
    (3U * sizeof(std::uint64_t)) + (2U * sizeof(std::uint32_t));
constexpr std::uint32_t arm_shared_text_base = 0x3000'0000U;
constexpr std::uint32_t arm_shared_data_base = 0x3800'0000U;
constexpr std::uint32_t arm_shared_region_end = 0x4000'0000U;
constexpr std::uint32_t arm_shared_half_size = 0x0800'0000U;
constexpr std::uint32_t vm_protection_copy_on_write = 0x08U;
constexpr std::uint32_t vm_protection_zero_fill = 0x10U;
constexpr std::uint32_t maximum_mapping_count =
    (2U * arm_shared_half_size) / AddressSpace::page_size;

struct Mapping {
  std::uint32_t address{};
  std::uint32_t size{};
  std::uint64_t file_offset{};
  std::uint32_t maximum_protection{};
  std::uint32_t initial_protection{};
};

struct AppliedMapping {
  std::uint32_t address{};
  std::uint32_t size{};
};

enum class MappingLayout {
  Arm32Words,
  MachVm64,
};

[[nodiscard]] bool add_overflows(std::uint32_t left, std::uint32_t right) {
  return right > std::numeric_limits<std::uint32_t>::max() - left;
}

[[nodiscard]] bool page_aligned(std::uint64_t value) {
  return value % AddressSpace::page_size == 0;
}

[[nodiscard]] bool in_arm_shared_region(std::uint32_t address,
                                        std::uint32_t size) {
  if (size == 0 || add_overflows(address, size)) return false;
  const auto end = address + size;
  return (address >= arm_shared_text_base && end <= arm_shared_data_base) ||
         (address >= arm_shared_data_base && end <= arm_shared_region_end);
}

[[nodiscard]] MemoryPermission permissions(std::uint32_t protection) {
  MemoryPermission result = MemoryPermission::None;
  if ((protection & 1U) != 0) result |= MemoryPermission::Read;
  if ((protection & 2U) != 0) result |= MemoryPermission::Write;
  if ((protection & 4U) != 0) result |= MemoryPermission::Execute;
  return result;
}

[[nodiscard]] std::optional<Mapping>
read_mapping(const AddressSpace &memory, std::uint32_t base,
             std::uint32_t index, MappingLayout layout) {
  const auto entry_size =
      layout == MappingLayout::Arm32Words ? arm_mapping_size
                                          : mach_vm_mapping_size;
  const auto offset = static_cast<std::uint64_t>(index) * entry_size;
  if (offset > std::numeric_limits<std::uint32_t>::max() - base) {
    return std::nullopt;
  }
  const auto address = base + static_cast<std::uint32_t>(offset);
  if (layout == MappingLayout::Arm32Words) {
    const auto mapping_address = memory.read32(address);
    const auto size = memory.read32(address + 4U);
    const auto file_offset = memory.read32(address + 8U);
    const auto maximum_protection = memory.read32(address + 12U);
    const auto initial_protection = memory.read32(address + 16U);
    if (!mapping_address || !size || !file_offset || !maximum_protection ||
        !initial_protection) {
      return std::nullopt;
    }
    return Mapping{*mapping_address, *size, *file_offset, *maximum_protection,
                   *initial_protection};
  }

  const auto mapping_address = memory.read64(address);
  const auto size = memory.read64(address + 8U);
  const auto file_offset = memory.read64(address + 16U);
  const auto maximum_protection = memory.read32(address + 24U);
  const auto initial_protection = memory.read32(address + 28U);
  if (!mapping_address || !size || !file_offset || !maximum_protection ||
      !initial_protection ||
      *mapping_address > std::numeric_limits<std::uint32_t>::max() ||
      *size > std::numeric_limits<std::uint32_t>::max()) {
    return std::nullopt;
  }
  return Mapping{static_cast<std::uint32_t>(*mapping_address),
                 static_cast<std::uint32_t>(*size), *file_offset,
                 *maximum_protection,
                 *initial_protection};
}

[[nodiscard]] bool valid_mapping(const Mapping &mapping,
                                 std::uintmax_t file_size) {
  return in_arm_shared_region(mapping.address, mapping.size) &&
         page_aligned(mapping.address) && page_aligned(mapping.file_offset) &&
         (mapping.initial_protection &
          ~(1U | 2U | 4U | vm_protection_copy_on_write |
            vm_protection_zero_fill)) == 0 &&
         (mapping.initial_protection & ~mapping.maximum_protection &
          (1U | 2U | 4U)) == 0 &&
         ((mapping.initial_protection & vm_protection_zero_fill) != 0 ||
          (mapping.file_offset <= file_size &&
           mapping.size <= file_size - mapping.file_offset));
}

[[nodiscard]] std::optional<std::vector<Mapping>>
read_mappings(const AddressSpace &memory, std::uint32_t address,
              std::uint32_t count, std::uintmax_t file_size,
              MappingLayout layout) {
  std::vector<Mapping> mappings;
  mappings.reserve(count);
  for (std::uint32_t index = 0; index < count; ++index) {
    const auto mapping = read_mapping(memory, address, index, layout);
    if (!mapping || !valid_mapping(*mapping, file_size)) {
      return std::nullopt;
    }
    mappings.push_back(*mapping);
  }
  return mappings;
}

[[nodiscard]] const char *layout_name(MappingLayout layout) {
  switch (layout) {
  case MappingLayout::Arm32Words:
    return "arm32";
  case MappingLayout::MachVm64:
    return "mach-vm64";
  }
  return "unknown";
}

[[nodiscard]] std::optional<std::uint32_t>
first_mapped_shared_region_page(const AddressSpace &memory) {
  for (std::uint64_t address = arm_shared_text_base;
       address < arm_shared_region_end; address += AddressSpace::page_size) {
    const auto page = static_cast<std::uint32_t>(address);
    if (memory.mapped(page, AddressSpace::page_size)) return page;
  }
  return std::nullopt;
}

[[nodiscard]] bool mappings_fit(const AddressSpace &memory,
                                const std::vector<Mapping> &mappings,
                                std::uint32_t slide) {
  for (const auto &mapping : mappings) {
    if (add_overflows(mapping.address, slide)) return false;
    const auto address = mapping.address + slide;
    if (!in_arm_shared_region(address, mapping.size) ||
        memory.mapped(address, mapping.size)) {
      return false;
    }
  }
  return true;
}

[[nodiscard]] std::optional<std::uint32_t>
choose_slide(const AddressSpace &memory, const std::vector<Mapping> &mappings,
             bool may_slide) {
  if (mappings_fit(memory, mappings, 0)) return 0;
  if (!may_slide || mappings.empty()) return std::nullopt;

  // XNU's map_shared_file() applies one slide to both ARM halves.  Search by
  // page so TEXT/DATA retain their 0x08000000 relationship.
  for (std::uint64_t slide = AddressSpace::page_size;
       slide < arm_shared_half_size; slide += AddressSpace::page_size) {
    if (mappings_fit(memory, mappings, static_cast<std::uint32_t>(slide))) {
      return static_cast<std::uint32_t>(slide);
    }
  }
  return std::nullopt;
}

void rollback(AddressSpace &memory,
              const std::vector<AppliedMapping> &mappings) {
  for (auto iterator = mappings.rbegin(); iterator != mappings.rend();
       ++iterator) {
    static_cast<void>(memory.unmap(iterator->address, iterator->size));
  }
}

} // namespace

bool CompatibilityKernel::dispatch_bsd_shared_region(Cpu &cpu,
                                                      std::uint32_t number) {
  auto &registers = cpu.registers();
  if (number == 294) { // shared_region_check_np
    const auto start_address = first_mapped_shared_region_page(memory_);
    if (!start_address) {
      bsd_error(cpu, 12); // ENOMEM: shared region exists but is empty.
    } else if (!memory_.write64(registers[0], *start_address)) {
      bsd_error(cpu, bsd_support::bad_address);
    } else {
      bsd_success(cpu, 0);
    }
    return true;
  }
  if (number == 300) { // shared_region_make_private_np
    // Each guest process currently owns its own vm_map and backing pages, so
    // detaching from the system map requires no page copy.  Keep the syscall
    // boundary because dyld uses it before retrying syscall 299 with a slide.
    bsd_success(cpu, 0);
    return true;
  }
  if (number != 299 && number != 295) return false;

  auto descriptor_number = registers[0];
  if (const auto duplicate = duplicated_descriptors_.find(descriptor_number);
      duplicate != duplicated_descriptors_.end()) {
    descriptor_number = duplicate->second;
  }
  const auto descriptor = file_descriptors_.find(descriptor_number);
  if (descriptor == file_descriptors_.end()) {
    bsd_error(cpu, bsd_support::bad_file_descriptor);
    return true;
  }
  const auto mapping_count = registers[1];
  const auto mappings_address = registers[2];
  const auto slide_address = number == 299 ? registers[3] : 0U;
  if (mapping_count == 0) {
    if (slide_address != 0 && !memory_.write64(slide_address, 0)) {
      bsd_error(cpu, bsd_support::bad_address);
    } else {
      bsd_success(cpu, 0);
    }
    return true;
  }
  if (mapping_count > maximum_mapping_count || mappings_address == 0) {
    bsd_error(cpu, bsd_support::invalid_argument);
    return true;
  }

  std::error_code file_error;
  const auto file_size = std::filesystem::file_size(descriptor->second,
                                                    file_error);
  if (file_error) {
    bsd_error(cpu, bsd_support::invalid_argument);
    return true;
  }

  auto layout = MappingLayout::Arm32Words;
  auto mappings =
      read_mappings(memory_, mappings_address, mapping_count, file_size, layout);
  if (!mappings) {
    layout = MappingLayout::MachVm64;
    mappings = read_mappings(memory_, mappings_address, mapping_count,
                             file_size, layout);
  }
  if (!mappings) {
    bsd_error(cpu, bsd_support::invalid_argument);
    return true;
  }

  const auto slide = choose_slide(memory_, *mappings, slide_address != 0);
  if (!slide) {
    bsd_error(cpu, 12); // ENOMEM / KERN_NO_SPACE
    return true;
  }

  std::vector<AppliedMapping> applied;
  applied.reserve(mappings->size());
  for (const auto &mapping : *mappings) {
    const auto address = mapping.address + *slide;
    const auto zero_fill =
        (mapping.initial_protection & vm_protection_zero_fill) != 0;
    const auto mapping_permissions =
        permissions(mapping.initial_protection);
    const auto mapped = zero_fill
                            ? memory_.map(
                                  address, mapping.size, mapping_permissions)
                            : memory_.map_file(
                                  address, mapping.size,
                                  mapping_permissions,
                                  descriptor->second, mapping.file_offset);
    if (!mapped) {
      rollback(memory_, applied);
      bsd_error(cpu, zero_fill ? 12 : bsd_support::invalid_argument);
      return true;
    }
    applied.push_back({address, mapping.size});
    if (*slide == 0U &&
        has_permission(mapping_permissions, MemoryPermission::Execute)) {
      memory_.mark_translation_profile_stable(address, mapping.size);
    }
  }

  if (slide_address != 0 && !memory_.write64(slide_address, *slide)) {
    rollback(memory_, applied);
    bsd_error(cpu, bsd_support::bad_address);
    return true;
  }
  for (const auto &mapping : *mappings) {
    if ((mapping.initial_protection & vm_protection_zero_fill) != 0) continue;
    static_cast<void>(install_mapped_user_image(
        cpu, descriptor->second, mapping.address + *slide, mapping.size,
        mapping.file_offset));
  }
  output_.write("[shared-region] map pid=" + std::to_string(process_.pid) +
                " file=" + descriptor->second.string() +
                " entries=" + std::to_string(mapping_count) +
                " layout=" + layout_name(layout) +
                " slide=" + std::to_string(*slide) + "\n");
  bsd_success(cpu, 0);
  return true;
}

} // namespace ilemu
