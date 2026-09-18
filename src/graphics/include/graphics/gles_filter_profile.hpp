// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace ilemu {

// Describes conventional texture filter expressions, independently of their
// caller. Offsets, weights and color columns remain firmware-owned uniforms.
struct GlesFilterProfile {
    enum class Operation { None, WeightedSamples, ColorMatrix, LuminanceAlpha };
    struct Tap {
        std::size_t offset_index { };
        float offset_sign { 1.0F };
        std::size_t weight_index { };
    };
    static constexpr std::size_t maximum_taps = 16U;
    Operation operation { Operation::None };
    std::string sampler;
    std::string coordinate_attribute;
    std::string coordinate_transform;
    std::string offsets;
    std::string weights;
    std::string color_matrix;
    std::vector<Tap> taps;

    [[nodiscard]] static GlesFilterProfile from_sources(
        std::string_view vertex, std::string_view fragment);
};

} // namespace ilemu
