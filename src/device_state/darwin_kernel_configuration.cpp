// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
#include "device_state/darwin_kernel_configuration.hpp"

#include "darwin_abi_profiles.hpp"
#include "darwin_firmware_identity.hpp"

#include <stdexcept>

namespace ilemu {

std::string_view darwin_abi_source_name(DarwinAbiSource source)
{
    switch (source) {
    case DarwinAbiSource::CompiledDefault:
        return "compiled-default";
    case DarwinAbiSource::FirmwareMetadata:
        return "firmware-metadata";
    case DarwinAbiSource::Explicit:
        return "explicit";
    case DarwinAbiSource::Unresolved:
        return "unresolved";
    }
    return "unresolved";
}

DarwinKernelConfiguration resolve_darwin_configuration(
    const std::filesystem::path& rootfs,
    std::optional<std::string_view> ios_build)
{
    DarwinKernelConfiguration configuration;
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
        configuration.abi_source = DarwinAbiSource::Explicit;
        configuration.abi_source_detail = build;
    } else if (!build.empty()) {
        selected = version ? darwin_configuration_for_build(*version) : nullptr;
        configuration.abi_source = selected ? DarwinAbiSource::FirmwareMetadata
                                            : DarwinAbiSource::Unresolved;
        configuration.abi_source_detail = build;
    } else if (rootfs.empty() || !std::filesystem::exists(rootfs)) {
        selected = &darwin_configurations().front();
        configuration.abi_source = DarwinAbiSource::CompiledDefault;
    }
    if (selected != nullptr) {
        configuration.abi = selected->abi;
        configuration.activation_profile = selected->activation_profile;
        configuration.identity =
            DarwinKernelIdentity { selected->darwin_release,
                build.empty() ? "1A543a" : std::string_view { build } };
        configuration.abi_name = selected->name;
        configuration.unlock_interaction = selected->unlock_interaction;
    } else {
        configuration.identity = DarwinKernelIdentity { "unknown",
            build.empty() ? "unknown" : std::string_view { build } };
    }
    return configuration;
}

} // namespace ilemu
