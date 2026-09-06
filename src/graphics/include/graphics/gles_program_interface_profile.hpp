// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// Classify compositor shader interfaces by their varying and uniform
// contracts.

#pragma once

#include <string_view>

namespace ilemu {

// Conventional compositor shaders expose either a single color varying or
// indexed color varyings and fragment outputs. Select from shader declarations,
// independently of the process, device or firmware that supplied the program.
struct GlesProgramInterfaceProfile {
    std::string_view color_attribute { "vertex_color" };
    std::string_view color_varying { "color" };
    std::string_view fragment_output { "gl_FragColor" };

    [[nodiscard]] static GlesProgramInterfaceProfile from_sources(
        std::string_view vertex, std::string_view fragment)
    {
        GlesProgramInterfaceProfile result;
        if (vertex.find("vertex_color0") != std::string_view::npos) {
            result.color_attribute = "vertex_color0";
            result.color_varying = "color0";
        }
        if (fragment.find("gl_FragData[0]") != std::string_view::npos)
            result.fragment_output = "gl_FragData[0]";
        return result;
    }
};

} // namespace ilemu
