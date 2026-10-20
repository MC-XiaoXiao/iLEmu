// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#pragma once

#include <GLES2/gl2.h>
#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace ilemu {

// Adapt the ES 1.00 single-color framebuffer input to a host texture without
// interpreting the firmware's fragment arithmetic.
struct EglFramebufferFetchSource {
    std::string fragment;
    std::string sampler;
    std::string inverse_size;
    std::string error;

    [[nodiscard]] static EglFramebufferFetchSource prepare(
        std::string_view source, bool native_fetch);
};

// A separate destination copy avoids texture/framebuffer feedback. Copies are
// made in primitive order so overlapping fragments observe preceding writes.
class EglFramebufferFetch {
public:
    void release();
    [[nodiscard]] bool draw(GLenum mode, std::uint32_t count,
        std::span<const std::uint16_t> indices, GLuint index_buffer,
        GLuint texture_unit, std::uint32_t width, std::uint32_t height,
        bool scissor_enabled, const std::array<std::int32_t, 4>& scissor,
        bool uint_indices);

private:
    GLuint texture_ { };
    std::uint32_t width_ { }, height_ { };
};

} // namespace ilemu
