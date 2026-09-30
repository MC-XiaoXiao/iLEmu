// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <span>

namespace ilemu::host_mig {
struct InformationResult {
    static constexpr std::size_t maximum_words = 42U;
    std::array<std::uint32_t, maximum_words> words { 0U, 1U };
    std::size_t count { 3U };
    explicit InformationResult(std::uint32_t error = 0U) { words[2] = error; }
    std::span<const std::uint32_t> payload() const { return std::span { words }.first(count); }
    static InformationResult counted(std::span<const std::uint32_t> values)
    {
        InformationResult result;
        result.words[3] = static_cast<std::uint32_t>(values.size());
        std::copy(values.begin(), values.end(), result.words.begin() + 4);
        result.count = 4U + values.size();
        return result;
    }
};
} // namespace ilemu::host_mig
