#include "ilemu/graphics_services_profile.hpp"

#include <bit>
#include <cstdint>
#include <string_view>

#include "ilemu/macho.hpp"

namespace ilemu {
namespace {

constexpr GraphicsServicesInputProfile darwin9_0_profile{
    .name = "darwin9.0",
    .event_record_size = 48,
    .record_timestamp_offset = 24,
    .record_info_size_offset = 44,
    .hand_info_size = 20,
    .path_info_size = 16,
    .hand_path_count_offset = 17,
    .path_location_offset = 8,
    .hand_phase_types = {1, 2, 6, 3},
    .path_phase_types = {2, 2, 2, 2},
    .system_events = {
        .home = {1000, 1001},
        .lock = {1010, 1011},
        .volume_up = {1006, 1007},
        .volume_down = {1008, 1009},
        .ringer_switch = {1012, 1013},
    },
};

constexpr GraphicsServicesInputProfile darwin9_3_profile{
    .name = "darwin9.3",
    .event_record_size = 48,
    .record_timestamp_offset = 24,
    .record_info_size_offset = 44,
    .hand_info_size = 36,
    .path_info_size = 24,
    .hand_path_count_offset = 33,
    .path_location_offset = 12,
    .hand_phase_types = {1, 2, 6, 3},
    .path_phase_types = {1, 2, 5, 3},
    .system_events = {
        .home = {1000, 1001},
        .lock = {1010, 1011},
        .volume_up = {1006, 1007},
        .volume_down = {1008, 1009},
        .ringer_switch = {1012, 1013},
    },
};

constexpr GraphicsServicesInputProfile darwin9_4_profile{
    .name = "darwin9.4",
    .event_record_size = 52,
    .record_timestamp_offset = 28,
    .record_info_size_offset = 48,
    .hand_info_size = 36,
    .path_info_size = 24,
    .hand_path_count_offset = 33,
    .path_location_offset = 12,
    .hand_phase_types = {1, 2, 6, 3},
    .path_phase_types = {1, 2, 5, 3},
    .system_events = {
        .home = {1000, 1001},
        .lock = {1010, 1011},
        .volume_up = {1006, 1007},
        .volume_down = {1008, 1009},
        .ringer_switch = {1012, 1013},
    },
};

std::uint32_t rotate_right(std::uint32_t value, std::uint32_t amount) {
  return std::rotr(value, static_cast<int>(amount));
}

std::optional<std::uint32_t>
structure_copy_size(const MachOImage &image, std::string_view symbol_name) {
  const auto *symbol = image.find_symbol(symbol_name);
  if (symbol == nullptr)
    return std::nullopt;

  // Accessors either call memcpy with the complete public structure size or
  // inline that copy as adjacent ARM LDM/STM pairs. Decode both compiler
  // forms instead of depending on a framework generation or symbol address.
  constexpr std::uint32_t mov_immediate_r2_mask = 0x0ffff000U;
  constexpr std::uint32_t mov_immediate_r2 = 0x03a02000U;
  constexpr std::uint32_t block_transfer_mask = 0x0e000000U;
  constexpr std::uint32_t block_transfer = 0x08000000U;
  constexpr std::uint32_t block_transfer_load = 0x00100000U;
  constexpr std::uint32_t register_list_mask = 0x0000ffffU;
  constexpr std::uint32_t maximum_accessor_size = 64U;
  std::uint32_t inline_copy_size = 0U;
  for (std::uint32_t offset = 0; offset != maximum_accessor_size;
       offset += 4) {
    const auto instruction = image.read_vm_u32(symbol->value + offset);
    if (!instruction)
      break;
    if ((*instruction & mov_immediate_r2_mask) == mov_immediate_r2) {
      const auto immediate = *instruction & 0xffU;
      const auto rotation = ((*instruction >> 8U) & 0xfU) * 2U;
      return rotate_right(immediate, rotation);
    }
    if ((*instruction & block_transfer_mask) != block_transfer ||
        (*instruction & block_transfer_load) == 0U) {
      continue;
    }
    const auto following = image.read_vm_u32(symbol->value + offset + 4U);
    if (!following ||
        (*following & block_transfer_mask) != block_transfer ||
        (*following & block_transfer_load) != 0U ||
        (*following & register_list_mask) !=
            (*instruction & register_list_mask)) {
      continue;
    }
    inline_copy_size +=
        static_cast<std::uint32_t>(
            std::popcount(*instruction & register_list_mask)) *
        static_cast<std::uint32_t>(sizeof(std::uint32_t));
    offset += 4U;
  }
  return inline_copy_size == 0U
             ? std::nullopt
             : std::optional<std::uint32_t>{inline_copy_size};
}

std::optional<std::uint32_t>
detected_event_record_size(const MachOImage &image) {
  const auto *symbol = image.find_symbol("_GSEventGetHandInfo");
  if (symbol == nullptr)
    return std::nullopt;

  // GSEventRef is a 32-bit CFRuntime object. The accessor advances its source
  // argument past CFRuntimeBase and GSEventRecord before copying HandInfo.
  // Decode that firmware-owned field offset and remove the stable runtime
  // header instead of assuming a public record layout.
  constexpr std::uint32_t cf_runtime_base_size = 8U;
  constexpr std::uint32_t add_immediate_r1_mask = 0x0ffff000U;
  constexpr std::uint32_t add_immediate_r1 = 0x02811000U;
  constexpr std::uint32_t maximum_prefix_size = 32U;
  for (std::uint32_t offset = 0; offset != maximum_prefix_size; offset += 4U) {
    const auto instruction = image.read_vm_u32(symbol->value + offset);
    if (!instruction ||
        (*instruction & add_immediate_r1_mask) != add_immediate_r1) {
      continue;
    }
    const auto immediate = *instruction & 0xffU;
    const auto rotation = ((*instruction >> 8U) & 0xfU) * 2U;
    const auto object_offset = rotate_right(immediate, rotation);
    if (object_offset >= cf_runtime_base_size)
      return object_offset - cf_runtime_base_size;
  }
  return std::nullopt;
}

} // namespace

std::uint8_t
GraphicsServicesInputProfile::hand_type(TouchPhase phase) const {
  return hand_phase_types[static_cast<std::size_t>(phase)];
}

std::uint8_t
GraphicsServicesInputProfile::path_type(TouchPhase phase) const {
  return path_phase_types[static_cast<std::size_t>(phase)];
}

std::uint32_t GraphicsServicesInputProfile::system_button_type(
    const SystemButtonInput &input) const {
  const auto index = input.phase == SystemButtonPhase::Down ? 0U : 1U;
  switch (input.button) {
  case SystemButton::Home:
    return system_events.home[index];
  case SystemButton::Lock:
    return system_events.lock[index];
  case SystemButton::VolumeUp:
    return system_events.volume_up[index];
  case SystemButton::VolumeDown:
    return system_events.volume_down[index];
  }
  return system_events.home[1];
}

std::uint32_t
GraphicsServicesInputProfile::ringer_switch_type(bool active) const {
  return system_events.ringer_switch[active ? 1U : 0U];
}

const GraphicsServicesInputProfile &GraphicsServicesInputProfile::for_abi(
    KernelSharedState::GraphicsInputAbi abi) {
  switch (abi) {
  case KernelSharedState::GraphicsInputAbi::Darwin9_4:
    return darwin9_4_profile;
  case KernelSharedState::GraphicsInputAbi::Darwin9_3:
    return darwin9_3_profile;
  case KernelSharedState::GraphicsInputAbi::LegacyMouse:
  case KernelSharedState::GraphicsInputAbi::Darwin9_0:
    return darwin9_0_profile;
  }
  return darwin9_0_profile;
}

std::optional<KernelSharedState::GraphicsInputAbi>
GraphicsServicesInputProfile::detect(const MachOImage &image) {
  const auto record_size = detected_event_record_size(image);
  const auto hand_size =
      structure_copy_size(image, "_GSEventGetHandInfo");
  const auto path_size =
      structure_copy_size(image, "_GSEventGetPathInfoAtIndex");
  if (record_size == darwin9_0_profile.event_record_size &&
      hand_size == darwin9_0_profile.hand_info_size &&
      path_size == darwin9_0_profile.path_info_size) {
    return KernelSharedState::GraphicsInputAbi::Darwin9_0;
  }
  if (record_size == darwin9_3_profile.event_record_size &&
      hand_size == darwin9_3_profile.hand_info_size &&
      path_size == darwin9_3_profile.path_info_size) {
    return KernelSharedState::GraphicsInputAbi::Darwin9_3;
  }
  if (record_size == darwin9_4_profile.event_record_size &&
      hand_size == darwin9_4_profile.hand_info_size &&
      path_size == darwin9_4_profile.path_info_size) {
    return KernelSharedState::GraphicsInputAbi::Darwin9_4;
  }
  return std::nullopt;
}

} // namespace ilemu
