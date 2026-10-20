// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "egl_framebuffer_fetch.hpp"

#include <algorithm>
#include <cctype>
#include <regex>
#include <vector>

namespace ilemu {
namespace {

    // Preserve offsets/newlines while excluding comments from source matching.
    std::string mask_comments(std::string_view source)
    {
        std::string result { source };
        for (std::size_t offset = 0; offset + 1U < source.size();) {
            const auto pair = source.substr(offset, 2U);
            if (pair != "//" && pair != "/*") {
                ++offset;
                continue;
            }
            auto end = pair == "//" ? source.find('\n', offset + 2U)
                                    : source.find("*/", offset + 2U);
            if (end == std::string_view::npos)
                end = source.size();
            else if (pair == "/*")
                end += 2U;
            for (; offset < end; ++offset)
                if (result[offset] != '\n')
                    result[offset] = ' ';
        }
        return result;
    }

    std::size_t preamble_end(std::string_view masked)
    {
        std::size_t offset { };
        while (offset < masked.size()) {
            if (std::isspace(static_cast<unsigned char>(masked[offset]))) {
                ++offset;
                continue;
            }
            if (masked[offset] != '#')
                break;
            auto end = masked.find('\n', offset);
            while (end != std::string_view::npos && end > 0U &&
                   masked[end - 1U] == '\\')
                end = masked.find('\n', end + 1U);
            offset = end == std::string_view::npos ? masked.size() : end + 1U;
        }
        return offset;
    }

} // namespace

EglFramebufferFetchSource EglFramebufferFetchSource::prepare(
    std::string_view source, bool native_fetch)
{
    EglFramebufferFetchSource result;
    result.fragment = source;
    if (native_fetch)
        return result;
    auto masked = mask_comments(source);
    static const std::regex extension(
        R"(#[ \t]*extension[ \t]+GL_EXT_shader_framebuffer_fetch[ \t]*:[ \t]*(?:require|enable|warn|disable)[^\n]*)");
    for (auto it =
             std::sregex_iterator(masked.begin(), masked.end(), extension);
        it != std::sregex_iterator(); ++it) {
        const auto offset = static_cast<std::size_t>(it->position());
        result.fragment.replace(offset, it->length(), it->length(), ' ');
    }
    static const std::regex identifier(R"(\bgl_LastFragData\b)");
    static const std::regex access(R"(gl_LastFragData\s*\[\s*0\s*\])");
    std::vector<std::pair<std::size_t, std::size_t>> reads;
    for (auto it =
             std::sregex_iterator(masked.begin(), masked.end(), identifier);
        it != std::sregex_iterator(); ++it) {
        const auto offset = static_cast<std::size_t>(it->position());
        std::smatch match;
        if (!std::regex_search(masked.cbegin() + offset, masked.cend(), match,
                access, std::regex_constants::match_continuous)) {
            result.error =
                "host framebuffer fetch requires color attachment zero";
            return result;
        }
        reads.emplace_back(offset, static_cast<std::size_t>(match.length()));
    }
    if (reads.empty())
        return result;
    std::string prefix { "ilemu_framebuffer_fetch" };
    while (masked.find(prefix) != std::string::npos)
        prefix += '_';
    result.sampler = prefix + "_color";
    result.inverse_size = prefix + "_inverse_size";
    const auto expression = "texture2D(" + result.sampler +
                            ", gl_FragCoord.xy * " + result.inverse_size + ")";
    for (auto it = reads.rbegin(); it != reads.rend(); ++it)
        result.fragment.replace(it->first, it->second, expression);
    const auto declarations = "\nuniform mediump sampler2D " + result.sampler +
                              ";\nuniform highp vec2 " + result.inverse_size +
                              ";\n";
    result.fragment.insert(
        preamble_end(mask_comments(result.fragment)), declarations);
    return result;
}

void EglFramebufferFetch::release()
{
    if (texture_)
        glDeleteTextures(1, &texture_);
    texture_ = 0;
    width_ = height_ = 0;
}

bool EglFramebufferFetch::draw(GLenum mode, std::uint32_t count,
    std::span<const std::uint16_t> indices, GLuint index_buffer,
    GLuint texture_unit, std::uint32_t width, std::uint32_t height,
    bool scissor_enabled, const std::array<std::int32_t, 4>& scissor,
    bool uint_indices)
{
    glActiveTexture(GL_TEXTURE0 + texture_unit);
    if (!texture_)
        glGenTextures(1, &texture_);
    glBindTexture(GL_TEXTURE_2D, texture_);
    if (width_ != width || height_ != height) {
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, static_cast<GLsizei>(width),
            static_cast<GLsizei>(height), 0, GL_RGBA, GL_UNSIGNED_BYTE,
            nullptr);
        width_ = width;
        height_ = height;
    }
    const auto left =
        scissor_enabled ? std::clamp<std::int64_t>(scissor[0], 0, width) : 0;
    const auto bottom =
        scissor_enabled ? std::clamp<std::int64_t>(scissor[1], 0, height) : 0;
    const auto right =
        scissor_enabled
            ? std::clamp<std::int64_t>(
                  std::int64_t { scissor[0] } + scissor[2], 0, width)
            : width;
    const auto top =
        scissor_enabled
            ? std::clamp<std::int64_t>(
                  std::int64_t { scissor[1] } + scissor[3], 0, height)
            : height;
    if (right <= left || top <= bottom)
        return true;
    const auto vertex = [&](std::uint32_t index) -> std::uint32_t {
        return indices.empty() ? index : indices[index];
    };
    if (!indices.empty())
        count = static_cast<std::uint32_t>(indices.size());
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, index_buffer);
    const auto primitive = [&](GLenum topology,
                               std::initializer_list<std::uint32_t> corners) {
        std::array<std::uint32_t, 3> wide { };
        std::array<std::uint16_t, 3> narrow { };
        bool needs_wide { };
        std::size_t index { };
        for (const auto corner : corners) {
            const auto value = vertex(corner);
            wide[index] = value;
            narrow[index++] = static_cast<std::uint16_t>(value);
            needs_wide |= value > 65535U;
        }
        if (needs_wide && !uint_indices)
            return false;
        glCopyTexSubImage2D(GL_TEXTURE_2D, 0, static_cast<GLint>(left),
            static_cast<GLint>(bottom), static_cast<GLint>(left),
            static_cast<GLint>(bottom), static_cast<GLsizei>(right - left),
            static_cast<GLsizei>(top - bottom));
        glBufferData(GL_ELEMENT_ARRAY_BUFFER,
            static_cast<GLsizeiptr>(index * (needs_wide ? 4U : 2U)),
            needs_wide ? static_cast<const void*>(wide.data())
                       : static_cast<const void*>(narrow.data()),
            GL_STREAM_DRAW);
        glDrawElements(topology, static_cast<GLsizei>(index),
            needs_wide ? GL_UNSIGNED_INT : GL_UNSIGNED_SHORT, nullptr);
        return true;
    };
    if (mode == GL_TRIANGLES) {
        for (std::uint32_t i = 0; i + 2U < count; i += 3U)
            if (!primitive(GL_TRIANGLES, { i, i + 1U, i + 2U }))
                return false;
    } else if (mode == GL_TRIANGLE_STRIP || mode == GL_TRIANGLE_FAN) {
        for (std::uint32_t i = 2; i < count; ++i) {
            const auto a = mode == GL_TRIANGLE_FAN ? 0U : i - 2U;
            const auto b = i - 1U;
            const auto odd = mode == GL_TRIANGLE_STRIP && (i & 1U);
            if (!primitive(GL_TRIANGLES, { odd ? b : a, odd ? a : b, i }))
                return false;
        }
    } else if (mode == GL_LINES || mode == GL_LINE_STRIP ||
               mode == GL_LINE_LOOP) {
        for (std::uint32_t i = 0; i + 1U < count;
            i += mode == GL_LINES ? 2U : 1U)
            if (!primitive(GL_LINES, { i, i + 1U }))
                return false;
        if (mode == GL_LINE_LOOP && count > 1U &&
            !primitive(GL_LINES, { count - 1U, 0U }))
            return false;
    } else {
        return false;
    }
    return glGetError() == GL_NO_ERROR;
}

} // namespace ilemu
