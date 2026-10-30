// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// Resolve firmware metadata into a session's Darwin configuration.

#pragma once

#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>

#include "device_state/darwin_build_version.hpp"
#include "device_state/darwin_kernel_identity.hpp"
#include "device_state/firmware_activation.hpp"
#include "foundation/darwin_version.hpp"
#include "foundation/device_input_profile.hpp"

namespace ilemu {

struct DarwinConfigurationEntry {
    std::string_view name;
    std::string_view darwin_release;
    DarwinVersion version;
    DarwinBuildVersion minimum_build;
    FirmwareActivation activation;
    UnlockInteraction unlock_interaction { UnlockInteraction::Slide };
};

enum class FirmwareSource {
    Unresolved,
    CompiledDefault,
    FirmwareMetadata,
    Explicit,
};

struct DarwinConfiguration {
    DarwinKernelIdentity identity;
    FirmwareActivation activation;
    UnlockInteraction unlock_interaction { UnlockInteraction::Slide };
    DarwinVersion version;
    std::string version_name { "unresolved" };
    FirmwareSource version_source { FirmwareSource::Unresolved };
    std::string version_source_detail;
};

[[nodiscard]] std::span<const DarwinConfigurationEntry> darwin_configurations();
[[nodiscard]] std::string_view darwin_version_source_name(
    FirmwareSource source);

// Resolve once per session and pass the immutable configuration to its
// processes. An explicit iOS build overrides firmware metadata for both
// the reported kernel identity and ABI selection.
[[nodiscard]] DarwinConfiguration resolve_darwin_configuration(
    const std::filesystem::path& rootfs,
    std::optional<std::string_view> ios_build = std::nullopt);

} // namespace ilemu
