// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
#pragma once

#include <compare>
#include <cstdint>

namespace ilemu {

// Values follow the numeric firmware build generation. There are no train or
// minor-release selectors in the emulator's compatibility model.
enum class DarwinGeneration : std::uint8_t {
    Unknown = 0,
    IphoneOs1 = 1,
    IphoneOs2 = 5,
    IphoneOs3 = 7,
    Ios4 = 8,
    Ios5 = 9,
    Ios6 = 10,
    Ios7 = 11,
    Ios8 = 12,
    Ios9 = 13,
    Ios10 = 14,
};

// One version replaces independently mutable feature ABI fields. Modules own
// their wire formats and derive them from this value or native protocol state.
class DarwinVersion {
public:
    constexpr DarwinVersion(
        DarwinGeneration generation = DarwinGeneration::Unknown)
        : generation_ { generation }
    {
    }

    [[nodiscard]] constexpr DarwinGeneration generation() const
    {
        return generation_;
    }
    [[nodiscard]] constexpr bool known() const
    {
        switch (generation_) {
        case DarwinGeneration::IphoneOs1:
        case DarwinGeneration::IphoneOs2:
        case DarwinGeneration::IphoneOs3:
        case DarwinGeneration::Ios4:
        case DarwinGeneration::Ios5:
        case DarwinGeneration::Ios6:
        case DarwinGeneration::Ios7:
        case DarwinGeneration::Ios8:
        case DarwinGeneration::Ios9:
        case DarwinGeneration::Ios10:
            return true;
        default:
            return false;
        }
    }
    constexpr auto operator<=>(const DarwinVersion&) const = default;
    constexpr bool operator==(const DarwinVersion&) const = default;
    constexpr auto operator<=>(DarwinGeneration other) const
    {
        return generation_ <=> other;
    }
    constexpr bool operator==(DarwinGeneration other) const
    {
        return generation_ == other;
    }

private:
    DarwinGeneration generation_;
};

} // namespace ilemu
