#pragma once

#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>

#include "device_state/darwin_abi.hpp"
#include "device_state/darwin_kernel_identity.hpp"

namespace ilemu {

struct DarwinConfigurationEntry {
    std::string_view name;
    std::string_view darwin_release;
    DarwinAbi abi;
};

enum class DarwinAbiSource {
    Unresolved,
    CompiledDefault,
    FirmwareMetadata,
    Explicit,
};

struct DarwinKernelConfiguration {
    DarwinKernelIdentity identity;
    DarwinAbi abi;
    std::string abi_name { "unresolved" };
    DarwinAbiSource abi_source { DarwinAbiSource::Unresolved };
    std::string abi_source_detail;
};

[[nodiscard]] std::span<const DarwinConfigurationEntry> darwin_configurations();
[[nodiscard]] std::string_view darwin_abi_source_name(DarwinAbiSource source);

// Resolve once per session and pass the immutable configuration to its
// processes. An explicit iOS build overrides firmware metadata for both
// the reported kernel identity and ABI selection.
[[nodiscard]] DarwinKernelConfiguration resolve_darwin_configuration(
    const std::filesystem::path& rootfs,
    std::optional<std::string_view> ios_build = std::nullopt);

} // namespace ilemu
