// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
#pragma once
#include "graphics/gles_program_renderer.hpp"

namespace ilemu {
[[nodiscard]] std::unique_ptr<GlesProgramRenderer> create_egl_program_renderer(
    std::string* error);
}
