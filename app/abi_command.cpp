// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
#include "app/abi_command.hpp"
#include "device_state/darwin_configuration.hpp"
#include "foundation/output.hpp"
#include <sstream>
#include <stdexcept>

namespace ilemu {
void inspect_abi(const std::optional<std::filesystem::path>& rootfs,
    const std::optional<std::string>& ios_build, Output& output, bool all)
{
    std::ostringstream text;
    if (all) {
        if (rootfs || ios_build)
            throw std::invalid_argument(
                "abi --all cannot be combined with --rootfs or --ios-build");
        text << "abi-catalog-schema: 2\n";
        for (const auto& entry : darwin_configurations()) {
            text << "\nabi: " << entry.name
                 << "\nbuild-generation: " << entry.minimum_build.generation
                 << "\nos-release: " << entry.darwin_release
                 << "\nactivation: " << entry.activation.name() << "\nunlock: "
                 << (entry.unlock_interaction == UnlockInteraction::PressHome
                            ? "press-home"
                            : "slide")
                 << '\n';
        }
    } else if (!rootfs && !ios_build) {
        text << "Use --rootfs DIR or --ios-build CODE to inspect a "
                "major-version configuration.\n";
        for (const auto& entry : darwin_configurations())
            text << "  " << entry.name << '\n';
    } else {
        const auto configuration = resolve_darwin_configuration(
            rootfs.value_or(std::filesystem::path { }), ios_build);
        text << "abi: " << configuration.version_name << "\nsource: "
             << darwin_version_source_name(configuration.version_source)
             << "\nsource-detail: " << configuration.version_source_detail
             << "\nactivation: " << configuration.activation.name()
             << "\nos-release: "
             << configuration.identity.operating_system_release
             << "\nos-version: " << configuration.identity.build_version
             << "\nkernel-version: " << configuration.identity.version << '\n';
    }
    output.write(text.str());
}
} // namespace ilemu
