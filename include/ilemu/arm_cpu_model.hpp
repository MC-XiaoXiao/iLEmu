#pragma once

#include <cstdint>
#include <memory>

namespace ilemu {

enum class ArmArchitectureVersion : std::uint8_t {
  Armv6K,
  Armv7,
};

enum class ArmCpuModelKind : std::uint8_t {
  Arm1176JzfS,
};

// A device-specific ARM core model. It selects both the decoder architecture
// and a static instruction-issue baseline. Dynarmic queries timing while
// translating a block and embeds the result in its cycle count, so this
// abstraction does not add a virtual call to execution of an already compiled
// block. Cache, TLB, pipeline dependency, and branch-predictor state remain
// intentionally outside the static baseline.
class ArmCpuModel {
public:
  virtual ~ArmCpuModel() = default;

  [[nodiscard]] virtual ArmArchitectureVersion
  architecture_version() const noexcept = 0;
  [[nodiscard]] virtual std::uint32_t ticks_per_second() const noexcept = 0;
  [[nodiscard]] virtual std::uint64_t
  ticks_for_instruction(bool thumb, std::uint32_t address,
                        std::uint32_t instruction) const noexcept = 0;
};

[[nodiscard]] std::unique_ptr<ArmCpuModel>
make_arm_cpu_model(ArmCpuModelKind kind, std::uint32_t clock_hz);

// Used by CPU-only helpers and tests that do not select a DeviceProfile.
[[nodiscard]] const ArmCpuModel &default_arm_cpu_model();

} // namespace ilemu
