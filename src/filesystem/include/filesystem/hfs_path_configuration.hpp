// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#pragma once

#include <cstdint>
#include <optional>

namespace ilemu::hfs {

// Darwin sys/unistd.h selectors and HFSX hfs_vnop_pathconf values.
// These are guest filesystem limits, independent of the host filesystem.
class PathConfiguration {
public:
    enum class Name : std::uint32_t {
        LinkMax = 1,
        NameMax = 4,
        PathMax = 5,
        PipeBuffer = 6,
        ChownRestricted = 7,
        NoTruncation = 8,
        NameCharsMax = 10,
        CaseSensitive = 11,
        CasePreserving = 12,
        FileSizeBits = 18,
        ExtendedAttributeSizeBits = 26,
    };

    [[nodiscard]] static constexpr std::optional<std::uint32_t> value(
        std::uint32_t name)
    {
        switch (static_cast<Name>(name)) {
        case Name::LinkMax: return 32767;
        case Name::NameMax:
        case Name::NameCharsMax: return 255;
        case Name::PathMax: return 1024;
        case Name::PipeBuffer: return 512;
        case Name::ChownRestricted:
        case Name::NoTruncation: return 200112;
        case Name::CaseSensitive:
        case Name::CasePreserving: return 1;
        case Name::FileSizeBits: return 64;
        case Name::ExtendedAttributeSizeBits: return 31;
        }
        return std::nullopt;
    }
};

} // namespace ilemu::hfs
