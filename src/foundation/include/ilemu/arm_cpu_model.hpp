#pragma once

#include <cstdint>
#include <memory>

namespace ilemu {

inline constexpr std::uint32_t arm_mach_cpu_type = 12U;

enum class ArmArchitectureVersion : std::uint8_t {
    Armv6K,
    Armv7,
};

enum class ArmCpuModelKind : std::uint8_t {
    Arm1176JzfS,
    CortexA8,
};

// ARM permits implementation-defined handling for instructions whose result
// is architecturally UNKNOWN. Keep that policy attached to the emulated core,
// independently of a device, firmware build, image, or guest address.
enum class ArmUnpredictableInstructionProfile : std::uint8_t {
    Strict,
    CortexA8,
};

[[nodiscard]] constexpr ArmArchitectureVersion arm_architecture_for_model(
    ArmCpuModelKind kind) noexcept
{
    switch (kind) {
    case ArmCpuModelKind::Arm1176JzfS:
        return ArmArchitectureVersion::Armv6K;
    case ArmCpuModelKind::CortexA8:
        return ArmArchitectureVersion::Armv7;
    }
    return ArmArchitectureVersion::Armv6K;
}

[[nodiscard]] constexpr std::uint32_t mach_cpu_subtype_for_architecture(
    ArmArchitectureVersion version) noexcept
{
    switch (version) {
    case ArmArchitectureVersion::Armv6K:
        return 6U; // CPU_SUBTYPE_ARM_V6
    case ArmArchitectureVersion::Armv7:
        return 9U; // CPU_SUBTYPE_ARM_V7
    }
    return 6U;
}

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
    [[nodiscard]] virtual ArmUnpredictableInstructionProfile
    unpredictable_instruction_profile() const noexcept = 0;
    [[nodiscard]] virtual std::uint32_t ticks_per_second() const noexcept = 0;
    [[nodiscard]] virtual std::uint64_t ticks_for_instruction(bool thumb,
        std::uint32_t address, std::uint32_t instruction) const noexcept = 0;
};

[[nodiscard]] std::unique_ptr<ArmCpuModel> make_arm_cpu_model(
    ArmCpuModelKind kind, std::uint32_t clock_hz);

// Used by CPU-only helpers and tests that do not select a DeviceProfile.
[[nodiscard]] const ArmCpuModel& default_arm_cpu_model();

} // namespace ilemu
