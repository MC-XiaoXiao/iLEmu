#include "ilemu/arm_unpredictable_instruction.hpp"

#include <cstddef>
#include <cstdint>

namespace ilemu {
namespace {

constexpr std::uint32_t asimd_vtrn_mask = 0xffb30f90U;
constexpr std::uint32_t asimd_vtrn_value = 0xf3b20080U;

[[nodiscard]] std::uint32_t asimd_instruction(bool thumb,
                                              std::uint32_t instruction) {
  if (!thumb)
    return instruction;
  // Thumb-2 Advanced SIMD keeps the low 24 bits of the ARM encoding and
  // moves U from bit 28 to bit 24.
  if ((instruction & 0xef000000U) != 0xef000000U)
    return 0U;
  const auto u = (instruction >> 28U) & 1U;
  return 0xf2000000U | (u << 24U) | (instruction & 0x00ffffffU);
}

} // namespace

bool emulate_arm_unpredictable_instruction(
    ArmUnpredictableInstructionProfile profile, bool thumb,
    std::uint32_t instruction,
    std::span<std::uint32_t, 64> extension_registers) noexcept {
  if (profile != ArmUnpredictableInstructionProfile::CortexA8)
    return false;

  const auto decoded = asimd_instruction(thumb, instruction);
  if ((decoded & asimd_vtrn_mask) != asimd_vtrn_value)
    return false;

  const auto element_size = (decoded >> 18U) & 0x3U;
  const auto q = ((decoded >> 6U) & 1U) != 0U;
  const auto destination = (((decoded >> 22U) & 1U) << 4U) |
                           ((decoded >> 12U) & 0xfU);
  const auto source = (((decoded >> 5U) & 1U) << 4U) |
                      (decoded & 0xfU);
  // The architecture marks equal source/destination VTRN results UNKNOWN.
  // Cortex-A8's 16-bit implementation retains each even lane as the final
  // write of the internal transpose pair. This behavior is also reproduced
  // by QEMU's Cortex-A8 model and is relied on by vectorized system codecs.
  if (element_size != 1U || destination != source ||
      (q && (destination & 1U) != 0U)) {
    return false;
  }

  const auto first_word = static_cast<std::size_t>(destination) * 2U;
  const auto word_count = q ? 4U : 2U;
  if (first_word + word_count > extension_registers.size())
    return false;
  for (std::size_t index = 0; index < word_count; ++index) {
    const auto even_lane = extension_registers[first_word + index] & 0xffffU;
    extension_registers[first_word + index] = even_lane | (even_lane << 16U);
  }
  return true;
}

} // namespace ilemu
