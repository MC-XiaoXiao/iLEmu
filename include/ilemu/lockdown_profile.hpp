#pragma once

#include <filesystem>
#include <optional>
#include <string_view>

#include "ilemu/arm_cpu_model.hpp"

namespace ilemu {

enum class LockdownActivation {
    Preserve,
    Activated,
    Unactivated,
};

struct LockdownFirmwareProfile {
    bool registration_state{true};
    bool brick_state{};
};

struct LockdownProfileResult {
    std::filesystem::path path;
    bool changed{};
};

[[nodiscard]] std::optional<LockdownActivation>
parse_lockdown_activation(std::string_view value);

// Select the Lockdown state contract from symbols imported by the firmware.
// This models API capabilities rather than product or build-version names.
[[nodiscard]] LockdownFirmwareProfile detect_lockdown_firmware_profile(
    const std::filesystem::path& rootfs,
    ArmArchitectureVersion architecture = ArmArchitectureVersion::Armv6K);

// data_ark.plist belongs to the simulated device's writable /var state, not
// the source firmware image. Seeding it models an already activated or factory
// device without emulating a baseband activation transaction.
[[nodiscard]] LockdownProfileResult apply_lockdown_profile(
    const std::filesystem::path& rootfs, LockdownActivation activation,
    const LockdownFirmwareProfile& profile);

}  // namespace ilemu
