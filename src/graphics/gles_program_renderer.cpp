// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "graphics/gles_program_renderer.hpp"

namespace ilemu {
namespace {
    GlesProgramRendererFactory program_renderer_factory { };
}
void configure_gles_program_renderer_factory(GlesProgramRendererFactory factory)
{
    program_renderer_factory = factory;
}
std::unique_ptr<GlesProgramRenderer> create_gles_program_renderer(
    std::string* error)
{
    if (program_renderer_factory)
        return program_renderer_factory(error);
    if (error)
        *error = "host GLES program compiler was not built";
    return { };
}
} // namespace ilemu
