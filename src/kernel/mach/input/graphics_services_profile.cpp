#include "ilemu/graphics_services_profile.hpp"

#include <bit>
#include <cstdint>
#include <string_view>

#include "ilemu/macho.hpp"

namespace ilemu {
namespace {

constexpr GraphicsServicesInputProfile darwin9_0_profile{
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

  // Both accessors finish by copying one complete public structure. Decode
  // their ARM `mov r2, #size` argument instead of depending on an address or
  // framework generation. Forty-eight bytes cover both known implementations
  // while remaining inside these small leaf functions.
  constexpr std::uint32_t mov_immediate_r2_mask = 0x0ffff000U;
  constexpr std::uint32_t mov_immediate_r2 = 0x03a02000U;
  for (std::uint32_t offset = 0; offset != 48; offset += 4) {
    const auto instruction = image.read_vm_u32(symbol->value + offset);
    if (!instruction ||
        (*instruction & mov_immediate_r2_mask) != mov_immediate_r2) {
      continue;
    }
    const auto immediate = *instruction & 0xffU;
    const auto rotation = ((*instruction >> 8U) & 0xfU) * 2U;
    return rotate_right(immediate, rotation);
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
  const auto hand_size =
      structure_copy_size(image, "_GSEventGetHandInfo");
  const auto path_size =
      structure_copy_size(image, "_GSEventGetPathInfoAtIndex");
  if (hand_size == darwin9_0_profile.hand_info_size &&
      path_size == darwin9_0_profile.path_info_size) {
    return KernelSharedState::GraphicsInputAbi::Darwin9_0;
  }
  if (hand_size == darwin9_3_profile.hand_info_size &&
      path_size == darwin9_3_profile.path_info_size) {
    return KernelSharedState::GraphicsInputAbi::Darwin9_3;
  }
  return std::nullopt;
}

} // namespace ilemu
