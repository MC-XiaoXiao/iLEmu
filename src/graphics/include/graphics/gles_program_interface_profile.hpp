// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// Classify compositor shader interfaces by their varying and uniform
// contracts.

#pragma once

#include "graphics/gles_abi.hpp"
#include "graphics/gles_filter_profile.hpp"

#include <array>
#include <string>
#include <string_view>

namespace ilemu {

enum class GlesFragmentOperation : std::uint32_t {
    TextureEnvironment,
    ColorDodge,
    PlusLighter,
};

// Conventional compositor shaders expose either a single color varying or
// indexed color varyings and fragment outputs. Select from shader declarations,
// independently of the process, device or firmware that supplied the program.
struct GlesProgramInterfaceProfile {
    GlesFilterProfile filter;
    enum class MatrixOrder {
        None,
        VectorTimesMatrix,
        MatrixTimesVector,
    };

    struct MatrixTextureInput {
        std::string attribute;
        std::string sampler;
        std::string vertex_matrix;
        std::string fragment_matrix;
        std::string color_uniform;
        MatrixOrder vertex_matrix_order { MatrixOrder::None };
        MatrixOrder fragment_matrix_order { MatrixOrder::None };
        bool rectangle { };

        [[nodiscard]] bool valid() const
        {
            return !attribute.empty() && !sampler.empty();
        }
    };

    struct ProjectedTextureInput {
        std::string sampler;
        std::string varying;
        std::string transform_uniform;
        bool rectangle { };

        [[nodiscard]] bool valid() const
        {
            return !sampler.empty() && !varying.empty() &&
                   !transform_uniform.empty();
        }
    };

    std::string position_attribute { "vertex_position" };
    std::string_view color_attribute { "vertex_color" };
    std::string_view color_varying { "color" };
    std::string_view fragment_output { "gl_FragColor" };
    std::array<MatrixTextureInput, gles_abi::texture_unit_count>
        matrix_texture_inputs;
    std::array<ProjectedTextureInput, gles_abi::texture_unit_count>
        projected_texture_inputs;
    GlesFragmentOperation fragment_operation {
        GlesFragmentOperation::TextureEnvironment
    };

    [[nodiscard]] static GlesProgramInterfaceProfile from_sources(
        std::string_view vertex, std::string_view fragment);
};

} // namespace ilemu
