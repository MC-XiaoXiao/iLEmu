#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>

#include "ilemu/kernel_shared_state.hpp"
#include "ilemu/touch_input.hpp"

namespace ilemu {

class MachOImage;

// Describes the firmware-owned structures appended to GSEventRecord. The
// profile is selected from the implementation of GraphicsServices' exported
// accessors, so it follows the ABI actually loaded by a process rather than a
// product version or build identifier.
struct GraphicsServicesInputProfile {
  std::size_t hand_info_size{};
  std::size_t path_info_size{};
  std::size_t hand_path_count_offset{};
  std::size_t path_location_offset{};
  bool path_carries_phase{};
  bool terminal_path_is_active{};
  std::array<std::uint8_t, 4> hand_phase_types{};
  std::array<std::uint8_t, 4> path_phase_types{};

  [[nodiscard]] std::uint8_t hand_type(TouchPhase phase) const;
  [[nodiscard]] std::uint8_t path_type(TouchPhase phase) const;

  [[nodiscard]] static const GraphicsServicesInputProfile &
  for_abi(KernelSharedState::GraphicsInputAbi abi);

  [[nodiscard]] static std::optional<KernelSharedState::GraphicsInputAbi>
  detect(const MachOImage &image);
};

} // namespace ilemu
