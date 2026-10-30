// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
#include "darwin_versions.hpp"

#include <array>

namespace ilemu {
namespace {
    constexpr FirmwareActivation development_activation {
        FirmwareActivation::Identity::DevelopmentBoard
    };
    constexpr FirmwareActivation retail_activation { };

    // A major generation accumulates the complete contract of that generation.
    // The build train and revision never select another compatibility variant.
    constexpr std::array configurations {
        DarwinConfigurationEntry { "iphoneos-1", "9.0.0d1",
            DarwinGeneration::IphoneOs1, { 1U }, development_activation },
        DarwinConfigurationEntry { "iphoneos-2", "9.4.1",
            DarwinGeneration::IphoneOs2, { 5U }, development_activation },
        DarwinConfigurationEntry { "iphoneos-3", "10.3.1",
            DarwinGeneration::IphoneOs3, { 7U }, development_activation },
        DarwinConfigurationEntry { "ios-4", "11.0.0", DarwinGeneration::Ios4,
            { 8U }, development_activation },
        DarwinConfigurationEntry { "ios-5", "11.0.0", DarwinGeneration::Ios5,
            { 9U }, retail_activation },
        DarwinConfigurationEntry { "ios-6", "13.0.0", DarwinGeneration::Ios6,
            { 10U }, retail_activation },
        DarwinConfigurationEntry { "ios-7", "14.0.0", DarwinGeneration::Ios7,
            { 11U }, retail_activation },
        DarwinConfigurationEntry { "ios-8", "14.0.0", DarwinGeneration::Ios8,
            { 12U }, retail_activation },
        DarwinConfigurationEntry { "ios-9", "15.0.0", DarwinGeneration::Ios9,
            { 13U }, retail_activation },
        DarwinConfigurationEntry { "ios-10", "16.0.0", DarwinGeneration::Ios10,
            { 14U }, retail_activation, UnlockInteraction::PressHome },
    };
    static_assert([] {
        for (std::size_t i = 1; i < configurations.size(); ++i)
            if (configurations[i - 1].minimum_build.generation >=
                configurations[i].minimum_build.generation)
                return false;
        return true;
    }());
} // namespace

std::span<const DarwinConfigurationEntry> darwin_configurations()
{
    return configurations;
}

const DarwinConfigurationEntry* darwin_configuration_for_build(
    DarwinBuildVersion build)
{
    if (build.generation < configurations.front().minimum_build.generation ||
        build.generation > configurations.back().minimum_build.generation)
        return nullptr;
    const DarwinConfigurationEntry* selected = nullptr;
    for (const auto& entry : configurations) {
        if (build.generation < entry.minimum_build.generation)
            break;
        selected = &entry;
    }
    return selected;
}

} // namespace ilemu
