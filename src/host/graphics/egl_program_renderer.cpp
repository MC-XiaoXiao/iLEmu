// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "egl_program_renderer.hpp"
#include "graphics/display.hpp"
#include "graphics/gles_resources.hpp"

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <algorithm>
#include <limits>
#include <mutex>
#include <stdexcept>

namespace ilemu {
namespace {

    struct EglDisplayOwner {
        EGLDisplay display { EGL_NO_DISPLAY };
        EGLConfig config { };
        EglDisplayOwner()
        {
            display = eglGetPlatformDisplay(
                EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, nullptr);
            if (display == EGL_NO_DISPLAY ||
                !eglInitialize(display, nullptr, nullptr))
                throw std::runtime_error {
                    "cannot initialize surfaceless EGL"
                };
            const EGLint attributes[] { EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
                EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT, EGL_RED_SIZE, 8,
                EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
                EGL_NONE };
            EGLint count { };
            if (!eglChooseConfig(display, attributes, &config, 1, &count) ||
                count == 0) {
                eglTerminate(display);
                throw std::runtime_error { "no host GLES2 configuration" };
            }
        }
        ~EglDisplayOwner() { eglTerminate(display); }
    };

    std::shared_ptr<EglDisplayOwner> shared_display()
    {
        static std::mutex mutex;
        static std::weak_ptr<EglDisplayOwner> current;
        std::lock_guard lock { mutex };
        auto display = current.lock();
        if (!display) {
            display = std::make_shared<EglDisplayOwner>();
            current = display;
        }
        return display;
    }

    class EglProgramRenderer final : public GlesProgramRenderer {
        struct Uniform {
            GLint location;
            GLenum type;
        };
        struct Program {
            GLuint object { };
            std::map<std::string, Uniform, std::less<>> uniforms;
        };
        struct Target {
            GLuint framebuffer { }, color { }, depth { };
            std::uint32_t width { }, height { };
            std::uint64_t depth_generation {
                std::numeric_limits<std::uint64_t>::max()
            };
            std::uint64_t last_used { };
            bool inverted_vertical { };
        };
        struct Texture {
            GLuint object { };
            std::map<std::uint32_t, std::uint64_t> revisions;
            std::map<std::uint32_t, std::size_t> sizes;
            std::uint64_t last_used { };
        };
        class Binding {
        public:
            explicit Binding(EglProgramRenderer& renderer)
                : renderer_ { renderer }
            {
                valid_ =
                    eglMakeCurrent(renderer_.display_, renderer_.surface_,
                        renderer_.surface_, renderer_.context_) == EGL_TRUE;
            }
            ~Binding()
            {
                if (valid_)
                    eglMakeCurrent(renderer_.display_, EGL_NO_SURFACE,
                        EGL_NO_SURFACE, EGL_NO_CONTEXT);
            }
            explicit operator bool() const { return valid_; }

        private:
            EglProgramRenderer& renderer_;
            bool valid_ { };
        };

    public:
        EglProgramRenderer()
            : display_owner_ { shared_display() }
        {
            try {
                display_ = display_owner_->display;
                eglBindAPI(EGL_OPENGL_ES_API);
                const EGLint context_attributes[] { EGL_CONTEXT_CLIENT_VERSION,
                    2, EGL_NONE };
                const EGLint surface_attributes[] { EGL_WIDTH, 1, EGL_HEIGHT, 1,
                    EGL_NONE };
                context_ = eglCreateContext(display_, display_owner_->config,
                    EGL_NO_CONTEXT, context_attributes);
                surface_ = eglCreatePbufferSurface(
                    display_, display_owner_->config, surface_attributes);
                if (context_ == EGL_NO_CONTEXT || surface_ == EGL_NO_SURFACE)
                    throw std::runtime_error {
                        "cannot create host GLES2 context"
                    };
                Binding binding { *this };
                if (!binding)
                    throw std::runtime_error {
                        "cannot bind host GLES context"
                    };
                const auto* renderer = glGetString(GL_RENDERER);
                name_ = renderer ? reinterpret_cast<const char*>(renderer)
                                 : "host GLES2";
                glGenBuffers(1, &attribute_buffer_);
                glGenBuffers(1, &index_buffer_);
            } catch (...) {
                if (surface_ != EGL_NO_SURFACE)
                    eglDestroySurface(display_, surface_);
                if (context_ != EGL_NO_CONTEXT)
                    eglDestroyContext(display_, context_);
                throw;
            }
        }
        ~EglProgramRenderer() override
        {
            if (context_ != EGL_NO_CONTEXT && surface_ != EGL_NO_SURFACE &&
                eglMakeCurrent(display_, surface_, surface_, context_)) {
                for (const auto& [key, program] : programs_) {
                    static_cast<void>(key);
                    glDeleteProgram(program.object);
                }
                for (const auto& [key, texture] : textures_) {
                    static_cast<void>(key);
                    glDeleteTextures(1, &texture.object);
                }
                for (const auto& [key, target] : targets_) {
                    static_cast<void>(key);
                    destroy(target);
                }
                glDeleteBuffers(1, &attribute_buffer_);
                glDeleteBuffers(1, &index_buffer_);
                eglMakeCurrent(
                    display_, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
            }
            if (surface_ != EGL_NO_SURFACE)
                eglDestroySurface(display_, surface_);
            if (context_ != EGL_NO_CONTEXT)
                eglDestroyContext(display_, context_);
        }
        LinkResult link(std::uint32_t name, std::string_view vertex,
            std::string_view fragment,
            const std::map<std::string, std::uint32_t, std::less<>>& bindings)
            override
        {
            std::lock_guard lock { mutex_ };
            LinkResult result;
            Binding binding { *this };
            if (!binding) {
                result.log = "cannot bind host GLES context";
                return result;
            }
            erase_bound(name);
            const auto compile = [&](GLenum type, std::string_view source) {
                const auto shader = glCreateShader(type);
                const auto* bytes = source.data();
                const auto size = static_cast<GLint>(source.size());
                glShaderSource(shader, 1, &bytes, &size);
                glCompileShader(shader);
                GLint success { };
                glGetShaderiv(shader, GL_COMPILE_STATUS, &success);
                if (!success) {
                    GLint length { };
                    glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &length);
                    std::string log(
                        static_cast<std::size_t>(std::max(length, 1)), '\0');
                    glGetShaderInfoLog(shader, length, nullptr, log.data());
                    result.log += log;
                    glDeleteShader(shader);
                    return GLuint { 0 };
                }
                return shader;
            };
            const auto vs = compile(GL_VERTEX_SHADER, vertex);
            const auto fs = compile(GL_FRAGMENT_SHADER, fragment);
            if (!vs || !fs) {
                if (vs)
                    glDeleteShader(vs);
                if (fs)
                    glDeleteShader(fs);
                return result;
            }
            Program program;
            program.object = glCreateProgram();
            glAttachShader(program.object, vs);
            glAttachShader(program.object, fs);
            for (const auto& [attribute, location] : bindings)
                glBindAttribLocation(
                    program.object, location, attribute.c_str());
            glLinkProgram(program.object);
            glDeleteShader(vs);
            glDeleteShader(fs);
            GLint linked { };
            glGetProgramiv(program.object, GL_LINK_STATUS, &linked);
            if (!linked) {
                GLint length { };
                glGetProgramiv(program.object, GL_INFO_LOG_LENGTH, &length);
                result.log.resize(
                    static_cast<std::size_t>(std::max(length, 1)));
                glGetProgramInfoLog(
                    program.object, length, nullptr, result.log.data());
                glDeleteProgram(program.object);
                return result;
            }
            GLint maximum { }, active { };
            glGetProgramiv(
                program.object, GL_ACTIVE_ATTRIBUTE_MAX_LENGTH, &maximum);
            glGetProgramiv(program.object, GL_ACTIVE_ATTRIBUTES, &active);
            std::string identifier(
                static_cast<std::size_t>(std::max(maximum, 1)), '\0');
            for (GLint index = 0; index < active; ++index) {
                GLsizei length { };
                GLint size { };
                GLenum type { };
                glGetActiveAttrib(program.object, static_cast<GLuint>(index),
                    maximum, &length, &size, &type, identifier.data());
                result.active_attributes.push_back(
                    { identifier.substr(0, static_cast<std::size_t>(length)),
                        type, static_cast<std::uint32_t>(size) });
                const auto location =
                    glGetAttribLocation(program.object, identifier.c_str());
                if (location >= 0)
                    result.attributes.emplace(
                        identifier.substr(0, static_cast<std::size_t>(length)),
                        static_cast<std::uint32_t>(location));
            }
            glGetProgramiv(
                program.object, GL_ACTIVE_UNIFORM_MAX_LENGTH, &maximum);
            glGetProgramiv(program.object, GL_ACTIVE_UNIFORMS, &active);
            identifier.resize(static_cast<std::size_t>(std::max(maximum, 1)));
            for (GLint index = 0; index < active; ++index) {
                GLsizei length { };
                GLint size { };
                GLenum type { };
                glGetActiveUniform(program.object, static_cast<GLuint>(index),
                    maximum, &length, &size, &type, identifier.data());
                const auto location =
                    glGetUniformLocation(program.object, identifier.c_str());
                auto key =
                    identifier.substr(0, static_cast<std::size_t>(length));
                result.active_uniforms.push_back(
                    { key, type, static_cast<std::uint32_t>(size) });
                if (key.ends_with("[0]"))
                    key.resize(key.size() - 3U);
                program.uniforms.emplace(
                    std::move(key), Uniform { location, type });
            }
            programs_.emplace(name, std::move(program));
            result.linked = true;
            return result;
        }
        void erase(std::uint32_t name) override
        {
            std::lock_guard lock { mutex_ };
            Binding binding { *this };
            if (binding)
                erase_bound(name);
        }
        bool contains(std::uint32_t name) const override
        {
            std::lock_guard lock { mutex_ };
            return programs_.contains(name);
        }
        std::optional<std::array<std::int32_t, 3>> precision(
            std::uint32_t shader, std::uint32_t type) override
        {
            std::lock_guard lock { mutex_ };
            Binding binding { *this };
            if (!binding)
                return std::nullopt;
            GLint range[2] { }, bits { };
            while (glGetError() != GL_NO_ERROR) { }
            glGetShaderPrecisionFormat(shader, type, range, &bits);
            if (glGetError() != GL_NO_ERROR)
                return std::nullopt;
            return std::array<std::int32_t, 3> { range[0], range[1], bits };
        }
        bool draw(DisplayFrame& frame, GlesRenderTargetKey key,
            const GlesProgramDraw& draw, std::uint32_t mode,
            const GlesRasterState& state) override
        {
            std::lock_guard lock { mutex_ };
            Binding binding { *this };
            if (!binding)
                return false;
            const auto program = programs_.find(draw.program);
            if (program == programs_.end() || !draw.state ||
                (!frame.pixels.empty() &&
                    frame.pixels.size() !=
                        static_cast<std::size_t>(frame.width) * frame.height))
                return false;
            while (glGetError() != GL_NO_ERROR) { }
            if (frame.pixels.empty() && !targets_.contains(key))
                return false;
            auto& target = targets_[key];
            target.inverted_vertical = state.render_target_inverted_vertical;
            target.last_used = ++use_sequence_;
            if (target.width != frame.width || target.height != frame.height) {
                destroy(target);
                target = { };
                target.last_used = use_sequence_;
                target.width = frame.width;
                target.height = frame.height;
                target.inverted_vertical =
                    state.render_target_inverted_vertical;
                glGenFramebuffers(1, &target.framebuffer);
                glGenTextures(1, &target.color);
                glBindTexture(GL_TEXTURE_2D, target.color);
                glTexParameteri(
                    GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
                glTexParameteri(
                    GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
                glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA,
                    static_cast<GLsizei>(frame.width),
                    static_cast<GLsizei>(frame.height), 0, GL_RGBA,
                    GL_UNSIGNED_BYTE, nullptr);
                glGenRenderbuffers(1, &target.depth);
                glBindRenderbuffer(GL_RENDERBUFFER, target.depth);
                glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT16,
                    static_cast<GLsizei>(frame.width),
                    static_cast<GLsizei>(frame.height));
                glBindFramebuffer(GL_FRAMEBUFFER, target.framebuffer);
                glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                    GL_TEXTURE_2D, target.color, 0);
                glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT,
                    GL_RENDERBUFFER, target.depth);
            }
            glBindFramebuffer(GL_FRAMEBUFFER, target.framebuffer);
            if (glCheckFramebufferStatus(GL_FRAMEBUFFER) !=
                GL_FRAMEBUFFER_COMPLETE)
                return false;
            if (!frame.pixels.empty()) {
                rgba_.resize(frame.pixels.size() * 4U);
                convert(frame.pixels, rgba_, frame.width, frame.height,
                    !state.render_target_inverted_vertical);
                glActiveTexture(GL_TEXTURE0);
                glBindTexture(GL_TEXTURE_2D, target.color);
                glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0,
                    static_cast<GLsizei>(frame.width),
                    static_cast<GLsizei>(frame.height), GL_RGBA,
                    GL_UNSIGNED_BYTE, rgba_.data());
            }
            glUseProgram(program->second.object);
            glViewport(state.viewport_x, state.viewport_y,
                static_cast<GLsizei>(state.viewport_width),
                static_cast<GLsizei>(state.viewport_height));
            set_enabled(GL_SCISSOR_TEST, state.scissor_enabled);
            glScissor(state.scissor_box[0], state.scissor_box[1],
                state.scissor_box[2], state.scissor_box[3]);
            set_enabled(GL_BLEND, state.blend_enabled);
            glBlendFunc(state.blend_source, state.blend_destination);
            set_enabled(GL_CULL_FACE, state.cull_enabled);
            glCullFace(state.cull_mode);
            glFrontFace(state.front_face);
            glColorMask(state.color_mask[0], state.color_mask[1],
                state.color_mask[2], state.color_mask[3]);
            glDepthMask(draw.depth_write);
            glDepthFunc(draw.depth_function);
            set_enabled(GL_DEPTH_TEST, draw.depth_test);
            if (target.depth_generation != draw.depth_generation) {
                glClearDepthf(draw.depth_clear);
                glClear(GL_DEPTH_BUFFER_BIT);
                target.depth_generation = draw.depth_generation;
            }
            for (const auto& [location, uniform] : draw.state->uniforms) {
                static_cast<void>(location);
                const auto active = program->second.uniforms.find(uniform.name);
                if (active == program->second.uniforms.end())
                    continue;
                upload_uniform(active->second, uniform);
            }
            for (std::size_t unit = 0; unit < draw.textures.size(); ++unit) {
                glActiveTexture(GL_TEXTURE0 + static_cast<GLenum>(unit));
                const auto* texture =
                    state.resources
                        ? state.resources->texture(draw.textures[unit])
                        : nullptr;
                if (!texture) {
                    glBindTexture(GL_TEXTURE_2D, 0);
                    continue;
                }
                auto& host = textures_[texture->name];
                host.last_used = use_sequence_;
                if (!host.object)
                    glGenTextures(1, &host.object);
                glBindTexture(GL_TEXTURE_2D, host.object);
                for (const auto& [level, image] : texture->levels) {
                    if (image.argb.size() !=
                        static_cast<std::size_t>(image.width) * image.height)
                        return false;
                    const auto previous = host.revisions.find(level);
                    if (previous != host.revisions.end() &&
                        previous->second == image.revision)
                        continue;
                    texture_rgba_.resize(image.argb.size() * 4U);
                    convert(image.argb, texture_rgba_, image.width,
                        image.height, false);
                    glTexImage2D(GL_TEXTURE_2D, static_cast<GLint>(level),
                        GL_RGBA, static_cast<GLsizei>(image.width),
                        static_cast<GLsizei>(image.height), 0, GL_RGBA,
                        GL_UNSIGNED_BYTE, texture_rgba_.data());
                    host.revisions[level] = image.revision;
                    host.sizes[level] =
                        image.argb.size() * sizeof(std::uint32_t);
                }
                for (const auto parameter :
                    { GL_TEXTURE_MIN_FILTER, GL_TEXTURE_MAG_FILTER,
                        GL_TEXTURE_WRAP_S, GL_TEXTURE_WRAP_T }) {
                    const auto value = texture->parameters.find(
                        static_cast<std::uint32_t>(parameter));
                    if (value != texture->parameters.end())
                        glTexParameteri(GL_TEXTURE_2D,
                            static_cast<GLenum>(parameter),
                            static_cast<GLint>(value->second));
                }
            }
            attributes_.clear();
            for (const auto& attribute : draw.attributes) {
                const auto bytes =
                    attribute.buffer.empty()
                        ? std::span<const std::byte> { attribute.client }
                        : attribute.buffer;
                attributes_.insert(
                    attributes_.end(), bytes.begin(), bytes.end());
                attributes_.resize(
                    (attributes_.size() + 3U) & ~std::size_t { 3U });
            }
            glBindBuffer(GL_ARRAY_BUFFER, attribute_buffer_);
            glBufferData(GL_ARRAY_BUFFER,
                static_cast<GLsizeiptr>(attributes_.size()), attributes_.data(),
                GL_STREAM_DRAW);
            for (GLuint index = 0; index < gles_abi::maximum_vertex_attributes;
                ++index)
                glDisableVertexAttribArray(index);
            std::size_t offset { };
            for (const auto& attribute : draw.attributes) {
                if (attribute.location >= gles_abi::maximum_vertex_attributes)
                    return false;
                if (!attribute.size) {
                    glVertexAttrib4fv(
                        attribute.location, attribute.value.data());
                    continue;
                }
                const auto bytes =
                    attribute.buffer.empty()
                        ? std::span<const std::byte> { attribute.client }
                        : attribute.buffer;
                glEnableVertexAttribArray(attribute.location);
                glVertexAttribPointer(attribute.location,
                    static_cast<GLint>(attribute.size), attribute.type,
                    attribute.normalized ? GL_TRUE : GL_FALSE,
                    static_cast<GLsizei>(attribute.stride),
                    reinterpret_cast<const void*>(offset));
                offset = (offset + bytes.size() + 3U) & ~std::size_t { 3U };
            }
            if (draw.indices.empty()) {
                glDrawArrays(mode, 0, static_cast<GLsizei>(draw.vertex_count));
            } else {
                glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, index_buffer_);
                glBufferData(GL_ELEMENT_ARRAY_BUFFER,
                    static_cast<GLsizeiptr>(
                        draw.indices.size() * sizeof(draw.indices.front())),
                    draw.indices.data(), GL_STREAM_DRAW);
                glDrawElements(mode, static_cast<GLsizei>(draw.indices.size()),
                    GL_UNSIGNED_SHORT, nullptr);
            }
            if (glGetError() != GL_NO_ERROR)
                return false;
            trim_caches();
            return true;
        }
        bool readback(DisplayFrame& frame, GlesRenderTargetKey key) override
        {
            std::lock_guard lock { mutex_ };
            Binding binding { *this };
            if (!binding)
                return false;
            const auto found = targets_.find(key);
            if (found == targets_.end())
                return false;
            const auto& target = found->second;
            frame.width = target.width;
            frame.height = target.height;
            frame.pixels.resize(
                static_cast<std::size_t>(frame.width) * frame.height);
            rgba_.resize(frame.pixels.size() * 4U);
            glBindFramebuffer(GL_FRAMEBUFFER, target.framebuffer);
            glReadPixels(0, 0, static_cast<GLsizei>(frame.width),
                static_cast<GLsizei>(frame.height), GL_RGBA, GL_UNSIGNED_BYTE,
                rgba_.data());
            if (glGetError() != GL_NO_ERROR)
                return false;
            for (std::uint32_t y = 0; y < frame.height; ++y) {
                const auto source_y =
                    target.inverted_vertical ? y : frame.height - 1U - y;
                for (std::uint32_t x = 0; x < frame.width; ++x) {
                    const auto offset_bytes =
                        (static_cast<std::size_t>(source_y) * frame.width + x) *
                        4U;
                    frame
                        .pixels[static_cast<std::size_t>(y) * frame.width + x] =
                        (static_cast<std::uint32_t>(rgba_[offset_bytes + 3U])
                            << 24U) |
                        (static_cast<std::uint32_t>(rgba_[offset_bytes])
                            << 16U) |
                        (static_cast<std::uint32_t>(rgba_[offset_bytes + 1U])
                            << 8U) |
                        rgba_[offset_bytes + 2U];
                }
            }
            return true;
        }
        std::string_view name() const override { return name_; }

    private:
        void trim_caches()
        {
            constexpr std::size_t maximum_targets = 8U;
            constexpr std::size_t texture_budget = 64U * 1024U * 1024U;
            while (targets_.size() > maximum_targets) {
                const auto oldest = std::min_element(targets_.begin(),
                    targets_.end(), [](const auto& a, const auto& b) {
                        return a.second.last_used < b.second.last_used;
                    });
                destroy(oldest->second);
                targets_.erase(oldest);
            }
            std::size_t bytes { };
            for (const auto& [name, texture] : textures_) {
                static_cast<void>(name);
                for (const auto& [level, size] : texture.sizes) {
                    static_cast<void>(level);
                    bytes += size;
                }
            }
            while (bytes > texture_budget && textures_.size() > 1U) {
                const auto oldest = std::min_element(textures_.begin(),
                    textures_.end(), [](const auto& a, const auto& b) {
                        return a.second.last_used < b.second.last_used;
                    });
                for (const auto& [level, size] : oldest->second.sizes) {
                    static_cast<void>(level);
                    bytes -= size;
                }
                glDeleteTextures(1, &oldest->second.object);
                textures_.erase(oldest);
            }
        }
        static void set_enabled(GLenum capability, bool enabled)
        {
            if (enabled)
                glEnable(capability);
            else
                glDisable(capability);
        }
        static void destroy(const Target& target)
        {
            if (target.framebuffer)
                glDeleteFramebuffers(1, &target.framebuffer);
            if (target.color)
                glDeleteTextures(1, &target.color);
            if (target.depth)
                glDeleteRenderbuffers(1, &target.depth);
        }
        void erase_bound(std::uint32_t name)
        {
            const auto found = programs_.find(name);
            if (found != programs_.end()) {
                glDeleteProgram(found->second.object);
                programs_.erase(found);
            }
        }
        static void convert(std::span<const std::uint32_t> pixels,
            std::vector<std::uint8_t>& output, std::uint32_t width,
            std::uint32_t height, bool flip)
        {
            for (std::uint32_t y = 0; y < height; ++y) {
                const auto row = flip ? height - 1U - y : y;
                for (std::uint32_t x = 0; x < width; ++x) {
                    const auto pixel =
                        pixels[static_cast<std::size_t>(row) * width + x];
                    const auto index =
                        (static_cast<std::size_t>(y) * width + x) * 4U;
                    output[index] = static_cast<std::uint8_t>(pixel >> 16U);
                    output[index + 1U] = static_cast<std::uint8_t>(pixel >> 8U);
                    output[index + 2U] = static_cast<std::uint8_t>(pixel);
                    output[index + 3U] =
                        static_cast<std::uint8_t>(pixel >> 24U);
                }
            }
        }
        static void upload_uniform(
            const Uniform& active, const GlesProgramState::Uniform& value)
        {
            if (value.integer) {
                glUniform1i(active.location, *value.integer);
                return;
            }
            if (value.values.empty())
                return;
            const auto* data = value.values.data();
            const auto count = value.value_count;
            switch (active.type) {
            case GL_FLOAT:
                glUniform1fv(
                    active.location, static_cast<GLsizei>(count), data);
                break;
            case GL_FLOAT_VEC2:
                glUniform2fv(
                    active.location, static_cast<GLsizei>(count / 2U), data);
                break;
            case GL_FLOAT_VEC3:
                glUniform3fv(
                    active.location, static_cast<GLsizei>(count / 3U), data);
                break;
            case GL_FLOAT_VEC4:
                glUniform4fv(
                    active.location, static_cast<GLsizei>(count / 4U), data);
                break;
            case GL_FLOAT_MAT2:
                glUniformMatrix2fv(active.location,
                    static_cast<GLsizei>(count / 4U), GL_FALSE, data);
                break;
            case GL_FLOAT_MAT3:
                glUniformMatrix3fv(active.location,
                    static_cast<GLsizei>(count / 9U), GL_FALSE, data);
                break;
            case GL_FLOAT_MAT4:
                glUniformMatrix4fv(active.location,
                    static_cast<GLsizei>(count / 16U), GL_FALSE, data);
                break;
            default:
                break;
            }
        }
        std::shared_ptr<EglDisplayOwner> display_owner_;
        EGLDisplay display_ { EGL_NO_DISPLAY };
        EGLContext context_ { EGL_NO_CONTEXT };
        EGLSurface surface_ { EGL_NO_SURFACE };
        GLuint attribute_buffer_ { };
        GLuint index_buffer_ { };
        std::string name_;
        mutable std::mutex mutex_;
        std::map<std::uint32_t, Program> programs_;
        std::map<std::uint32_t, Texture> textures_;
        std::map<GlesRenderTargetKey, Target> targets_;
        std::vector<std::uint8_t> rgba_, texture_rgba_;
        std::vector<std::byte> attributes_;
        std::uint64_t use_sequence_ { };
    };

} // namespace
std::unique_ptr<GlesProgramRenderer> create_egl_program_renderer(
    std::string* error)
{
    try {
        return std::make_unique<EglProgramRenderer>();
    } catch (const std::exception& exception) {
        if (error)
            *error = exception.what();
        return { };
    }
}
} // namespace ilemu
