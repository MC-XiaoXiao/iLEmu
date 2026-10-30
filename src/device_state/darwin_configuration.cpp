// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
#include "device_state/darwin_configuration.hpp"

#include "darwin_firmware_identity.hpp"
#include "darwin_versions.hpp"

#include <stdexcept>

namespace ilemu {

std::string_view darwin_version_source_name(FirmwareSource source)
{
    switch (source) {
    case FirmwareSource::CompiledDefault:
        return "compiled-default";
    case FirmwareSource::FirmwareMetadata:
        return "firmware-metadata";
    case FirmwareSource::Explicit:
        return "explicit";
    case FirmwareSource::Unresolved:
        return "unresolved";
    }
    return "unresolved";
}

DarwinConfiguration resolve_darwin_configuration(
    const std::filesystem::path& rootfs,
    std::optional<std::string_view> ios_build)
{
    DarwinConfiguration configuration;
    const auto build = ios_build ? std::string { *ios_build }
                                 : read_darwin_build_version(rootfs);
    const auto version = DarwinBuildVersion::parse(build);
    const DarwinConfigurationEntry* selected = nullptr;
    if (ios_build) {
        if (!version) {
            throw std::invalid_argument { "invalid iOS build code: " + build +
                                          "; expected a code such as 9A334" };
        }
        selected = darwin_configuration_for_build(*version);
        if (selected == nullptr) {
            throw std::invalid_argument { "unsupported iOS build code: " +
                                          build };
        }
        configuration.version_source = FirmwareSource::Explicit;
        configuration.version_source_detail = build;
    } else if (!build.empty()) {
        selected = version ? darwin_configuration_for_build(*version) : nullptr;
        configuration.version_source = selected
                                           ? FirmwareSource::FirmwareMetadata
                                           : FirmwareSource::Unresolved;
        configuration.version_source_detail = build;
    } else if (rootfs.empty() || !std::filesystem::exists(rootfs)) {
        selected = &darwin_configurations().front();
        configuration.version_source = FirmwareSource::CompiledDefault;
    }
    if (selected != nullptr) {
        configuration.version = selected->version;
        configuration.activation = selected->activation;
        configuration.identity =
            DarwinKernelIdentity { selected->darwin_release,
                build.empty() ? "1A543a" : std::string_view { build } };
        configuration.version_name = selected->name;
        configuration.unlock_interaction = selected->unlock_interaction;
    } else {
        configuration.identity = DarwinKernelIdentity { "unknown",
            build.empty() ? "unknown" : std::string_view { build } };
    }
    return configuration;
}

} // namespace ilemu
