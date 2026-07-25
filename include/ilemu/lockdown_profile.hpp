#pragma once

#include <filesystem>
#include <optional>
#include <string_view>

namespace ilemu {

enum class LockdownActivation {
    Preserve,
    Activated,
    Unactivated,
};

struct LockdownProfileResult {
    std::filesystem::path path;
    bool changed{};
};

[[nodiscard]] std::optional<LockdownActivation>
parse_lockdown_activation(std::string_view value);

// data_ark.plist belongs to the simulated device's writable /var state, not
// the source firmware image. Seeding it models an already activated or factory
// device without emulating a baseband activation transaction.
[[nodiscard]] LockdownProfileResult apply_lockdown_profile(
    const std::filesystem::path& rootfs, LockdownActivation activation);

}  // namespace ilemu
