#include "ilemu/opengles_hle.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>

#include "ilemu/address_space.hpp"
#include "ilemu/core_surface_abi.hpp"
#include "ilemu/cpu.hpp"
#include "ilemu/display.hpp"
#include "ilemu/kernel_shared_state.hpp"
#include "ilemu/output.hpp"
#include "ilemu/scene_coordinator.hpp"
#include "ilemu/surface_store.hpp"
#include "ilemu/userland_hle.hpp"

namespace ilemu {
namespace {

constexpr std::string_view opengles_image{
    "/System/Library/Frameworks/OpenGLES.framework/OpenGLES"};

constexpr std::uint32_t egl_false = 0;
constexpr std::uint32_t egl_true = 1;
constexpr std::uint32_t egl_default_display = 1;
constexpr std::uint32_t egl_default_config = 1;
constexpr std::uint32_t egl_success = 0x3000;
constexpr std::uint32_t egl_bad_display = 0x3008;
constexpr std::uint32_t egl_bad_parameter = 0x300c;
constexpr std::uint32_t egl_bad_surface = 0x300d;
constexpr std::uint32_t egl_bad_context = 0x3006;

constexpr std::uint32_t egl_buffer_size = 0x3020;
constexpr std::uint32_t egl_alpha_size = 0x3021;
constexpr std::uint32_t egl_blue_size = 0x3022;
constexpr std::uint32_t egl_green_size = 0x3023;
constexpr std::uint32_t egl_red_size = 0x3024;
constexpr std::uint32_t egl_depth_size = 0x3025;
constexpr std::uint32_t egl_stencil_size = 0x3026;
constexpr std::uint32_t egl_config_id = 0x3028;
constexpr std::uint32_t egl_surface_type = 0x3033;
constexpr std::uint32_t egl_vendor = 0x3053;
constexpr std::uint32_t egl_version = 0x3054;
constexpr std::uint32_t egl_extensions = 0x3055;
constexpr std::uint32_t egl_height = 0x3056;
constexpr std::uint32_t egl_width = 0x3057;
constexpr std::uint32_t egl_renderable_type = 0x3040;
constexpr std::uint32_t egl_context_client_version = 0x3098;

constexpr std::uint32_t egl_window_bit = 0x0004;
constexpr std::uint32_t egl_pbuffer_bit = 0x0001;
constexpr std::uint32_t egl_opengl_es_bit = 0x0001;

constexpr std::uint32_t gl_no_error = gles_abi::no_error;
constexpr std::uint32_t gl_invalid_value = gles_abi::invalid_value;
constexpr std::uint32_t gl_vendor = 0x1f00;
constexpr std::uint32_t gl_renderer = 0x1f01;
constexpr std::uint32_t gl_version = 0x1f02;
constexpr std::uint32_t gl_extensions = 0x1f03;
constexpr std::size_t maximum_unsupported_traces = 64;

bool is_valid_display(std::uint32_t display) {
    return display == egl_default_display;
}

} // namespace

OpenGlesHle::OpenGlesHle(UserlandHleRegistry& registry,
                         std::shared_ptr<DisplayState> display,
                         std::shared_ptr<SurfaceStore> surfaces)
    : display_{std::move(display)},
      surface_store_{surfaces ? std::move(surfaces)
                              : std::make_shared<SurfaceStore>()},
      renderer_owner_{allocate_gles_renderer_owner()},
      renderer_{shared_gles_renderer()},
      command_encoder_{renderer_->create_command_encoder()} {
    register_egl(registry);
    register_gles(registry);
}

OpenGlesHle::~OpenGlesHle() {
    release_renderer_resources();
}

void OpenGlesHle::reset() {
    release_renderer_resources();
    threads_.clear();
    contexts_.clear();
    surfaces_.clear();
    resources_.reset();
    renderer_owner_ = allocate_gles_renderer_owner();
    next_context_ = 0x00010001U;
    next_surface_ = 0x00020001U;
    egl_error_ = egl_success;
    frame_count_ = 0;
    unsupported_trace_count_ = 0;
}

void OpenGlesHle::inherit_state(const OpenGlesHle& parent) {
    release_renderer_resources();
    threads_ = parent.threads_;
    contexts_ = parent.contexts_;
    surfaces_ = parent.surfaces_;
    resources_.inherit_state(parent.resources_);
    renderer_owner_ = allocate_gles_renderer_owner();
    next_context_ = parent.next_context_;
    next_surface_ = parent.next_surface_;
    egl_error_ = parent.egl_error_;
    frame_count_ = parent.frame_count_;
}

void OpenGlesHle::release_renderer_resources() {
    if (renderer_owner_ != 0)
        renderer_->release_owner(renderer_owner_);
}

void OpenGlesHle::set_display(std::shared_ptr<DisplayState> display) {
    display_ = std::move(display);
}

void OpenGlesHle::set_shared_state(
    std::shared_ptr<KernelSharedState> shared_state) {
    shared_state_ = std::move(shared_state);
}

void OpenGlesHle::set_scene_coordinator(
    std::shared_ptr<SceneCoordinator> scenes) {
    scene_coordinator_ = std::move(scenes);
}

bool OpenGlesHle::display_write_allowed(UserlandHleCall& call) const {
    if (!shared_state_)
        return true;
    std::lock_guard lock{shared_state_->mach_mutex};
    const auto process = shared_state_->processes.find(call.process_id());
    if (process == shared_state_->processes.end() ||
        !process->second.executable_path.starts_with("/Applications/")) {
        return true;
    }
    return active_application_owns_display_locked(
        *shared_state_, call.process_id(),
        scene_coordinator_
            ? std::optional<bool>{
                  scene_coordinator_->client_scene_presentable(
                      call.process_id())}
            : std::nullopt);
}

OpenGlesHle::ThreadState& OpenGlesHle::thread(UserlandHleCall& call) {
    return threads_[call.cpu().processor_id()];
}

OpenGlesHle::ContextState* OpenGlesHle::current_context(UserlandHleCall& call) {
    const auto context = contexts_.find(thread(call).context);
    return context == contexts_.end() ? nullptr : &context->second;
}

void OpenGlesHle::set_gl_error(UserlandHleCall& call, std::uint32_t error) {
    auto& value = thread(call).gl_error;
    if (value != gles_abi::no_error)
        return;
    value = error;
}

GlesMatrix* OpenGlesHle::current_matrix(ContextState& context) {
    if (context.matrix_mode == gles_abi::modelview) {
        return &context.modelview_matrix;
    }
    if (context.matrix_mode == gles_abi::projection) {
        return &context.projection_matrix;
    }
    if (context.matrix_mode == gles_abi::texture_matrix) {
        return &context.texture_units[context.active_texture_unit]
                    .texture_matrix;
    }
    return nullptr;
}

std::vector<GlesMatrix>*
OpenGlesHle::current_matrix_stack(ContextState& context) {
    if (context.matrix_mode == gles_abi::modelview) {
        return &context.modelview_stack;
    }
    if (context.matrix_mode == gles_abi::projection) {
        return &context.projection_stack;
    }
    if (context.matrix_mode == gles_abi::texture_matrix) {
        return &context.texture_units[context.active_texture_unit]
                    .texture_stack;
    }
    return nullptr;
}

void OpenGlesHle::multiply_current_matrix(UserlandHleCall& call,
                                          GlesMatrix matrix) {
    auto* context = current_context(call);
    if (context == nullptr) {
        set_gl_error(call, gles_abi::invalid_operation);
        return;
    }
    auto* current = current_matrix(*context);
    if (current == nullptr) {
        set_gl_error(call, gles_abi::invalid_operation);
        return;
    }
    *current = *current * matrix;
}

void OpenGlesHle::set_array_pointer(UserlandHleCall& call,
                                    std::uint32_t array_name) {
    auto* context = current_context(call);
    if (context == nullptr) {
        set_gl_error(call, gles_abi::invalid_operation);
        return;
    }
    const auto size = static_cast<std::int32_t>(call.argument(0));
    const auto type = call.argument(1);
    const auto stride = static_cast<std::int32_t>(call.argument(2));
    const auto valid_common_type = type == gles_abi::float_type ||
                                   type == gles_abi::fixed ||
                                   type == gles_abi::short_type;
    bool valid = stride >= 0;
    if (array_name == gles_abi::vertex_array) {
        valid = valid && size >= 2 && size <= 4 && valid_common_type;
    } else if (array_name == gles_abi::color_array) {
        valid = valid && size == 4 &&
                (valid_common_type || type == gles_abi::unsigned_byte);
    } else {
        valid = valid && size >= 2 && size <= 4 && valid_common_type;
    }
    if (!valid) {
        set_gl_error(call, stride < 0 ? gles_abi::invalid_value
                                      : gles_abi::invalid_enum);
        return;
    }
    auto* array =
        array_name == gles_abi::vertex_array ? &context->vertex_array
        : array_name == gles_abi::color_array
            ? &context->color_array
            : &context->texture_units[context->client_active_texture_unit]
                   .texture_array;
    const auto enabled = array->enabled;
    *array = ContextState::ArrayPointer{
        static_cast<std::uint32_t>(size),   type,
        static_cast<std::uint32_t>(stride), call.argument(3),
        context->bound_array_buffer,        enabled};
}

bool OpenGlesHle::read_array(UserlandHleCall& call,
                             const ContextState::ArrayPointer& array,
                             std::uint32_t index, std::span<float> destination,
                             bool normalized) const {
    std::uint32_t component_size{};
    if (array.type == gles_abi::float_type || array.type == gles_abi::fixed) {
        component_size = 4;
    } else if (array.type == gles_abi::short_type) {
        component_size = 2;
    } else if (array.type == gles_abi::byte ||
               array.type == gles_abi::unsigned_byte) {
        component_size = 1;
    } else {
        return false;
    }
    if (array.size > destination.size())
        return false;
    const auto stride =
        array.stride == 0 ? array.size * component_size : array.stride;
    const auto base_offset = static_cast<std::uint64_t>(array.pointer) +
                             static_cast<std::uint64_t>(index) * stride;
    const auto* buffer =
        array.buffer == 0 ? nullptr : resources_.buffer(array.buffer);
    if (array.buffer != 0 && buffer == nullptr)
        return false;
    for (std::uint32_t component = 0; component < array.size; ++component) {
        const auto offset =
            base_offset +
            static_cast<std::uint64_t>(component) * component_size;
        std::uint32_t raw{};
        if (buffer != nullptr) {
            if (offset > buffer->bytes.size() ||
                component_size > buffer->bytes.size() - offset) {
                return false;
            }
            for (std::uint32_t byte_index = 0; byte_index < component_size;
                 ++byte_index) {
                raw |= std::to_integer<std::uint32_t>(
                           buffer->bytes[static_cast<std::size_t>(offset) +
                                         byte_index])
                       << (byte_index * 8U);
            }
        } else {
            if (offset > std::numeric_limits<std::uint32_t>::max())
                return false;
            const auto address = static_cast<std::uint32_t>(offset);
            if (component_size == 4) {
                const auto value = call.memory().read32(address);
                if (!value)
                    return false;
                raw = *value;
            } else if (component_size == 2) {
                const auto value = call.memory().read16(address);
                if (!value)
                    return false;
                raw = *value;
            } else {
                const auto value = call.memory().read8(address);
                if (!value)
                    return false;
                raw = *value;
            }
        }
        float value{};
        if (array.type == gles_abi::float_type) {
            value = std::bit_cast<float>(raw);
        } else if (array.type == gles_abi::fixed) {
            value =
                static_cast<float>(static_cast<std::int32_t>(raw)) / 65'536.0F;
        } else if (array.type == gles_abi::short_type) {
            const auto signed_value = static_cast<std::int16_t>(raw);
            value = normalized
                        ? std::max(-1.0F,
                                   static_cast<float>(signed_value) / 32'767.0F)
                        : static_cast<float>(signed_value);
        } else if (array.type == gles_abi::byte) {
            const auto signed_value = static_cast<std::int8_t>(raw);
            value =
                normalized
                    ? std::max(-1.0F, static_cast<float>(signed_value) / 127.0F)
                    : static_cast<float>(signed_value);
        } else {
            value = normalized ? static_cast<float>(raw) / 255.0F
                               : static_cast<float>(raw);
        }
        destination[component] = value;
    }
    return true;
}

std::optional<GlesRasterVertex>
OpenGlesHle::read_vertex(UserlandHleCall& call, const ContextState& context,
                         std::uint32_t index) const {
    if (!context.vertex_array.enabled)
        return std::nullopt;
    GlesRasterVertex vertex;
    vertex.color = context.current_color;
    if (!read_array(call, context.vertex_array, index, vertex.position,
                    false)) {
        return std::nullopt;
    }
    if (context.color_array.enabled &&
        !read_array(call, context.color_array, index, vertex.color, true)) {
        return std::nullopt;
    }
    vertex.position = context.projection_matrix.transform(
        context.modelview_matrix.transform(vertex.position));
    for (std::size_t unit_index = 0; unit_index < context.texture_units.size();
         ++unit_index) {
        const auto& unit = context.texture_units[unit_index];
        if (unit.texture_array.enabled) {
            std::array<float, 4> texture{};
            if (!read_array(call, unit.texture_array, index, texture, false)) {
                return std::nullopt;
            }
            vertex.texture[unit_index] = {texture[0], texture[1]};
        }
        const auto transformed_texture = unit.texture_matrix.transform(
            {vertex.texture[unit_index][0], vertex.texture[unit_index][1], 0.0F,
             1.0F});
        if (transformed_texture[3] != 0.0F) {
            vertex.texture[unit_index] = {
                transformed_texture[0] / transformed_texture[3],
                transformed_texture[1] / transformed_texture[3]};
        }
    }
    return vertex;
}

std::optional<std::uint32_t>
OpenGlesHle::core_surface_identifier(UserlandHleCall& call,
                                     std::uint32_t surface) const {
    if (surface == 0 ||
        surface > std::numeric_limits<std::uint32_t>::max() -
                      core_surface_abi::public_client_buffer_offset) {
        return std::nullopt;
    }
    const auto client = call.memory().read32(
        surface + core_surface_abi::public_client_buffer_offset);
    if (!client || *client == 0 ||
        *client > std::numeric_limits<std::uint32_t>::max() -
                      core_surface_abi::client_identifier_offset) {
        return std::nullopt;
    }
    const auto identifier = call.memory().read32(
        *client + core_surface_abi::client_identifier_offset);
    return identifier && *identifier != 0
               ? std::optional<std::uint32_t>{*identifier}
               : std::nullopt;
}

OpenGlesHle::SurfaceState*
OpenGlesHle::current_pixmap_surface(UserlandHleCall& call) {
    const auto found = surfaces_.find(thread(call).draw_surface);
    return found != surfaces_.end() &&
                   found->second.backing_identifier.has_value()
               ? &found->second
               : nullptr;
}

GlesRenderTargetKey
OpenGlesHle::render_target_key(std::uint32_t surface) const {
    const auto found = surfaces_.find(surface);
    if (found != surfaces_.end() && found->second.backing_identifier) {
        const auto backing =
            surface_store_->find(*found->second.backing_identifier);
        if (backing)
            return {0, backing->provenance.publication_sequence};
    }
    return {renderer_owner_, surface};
}

bool OpenGlesHle::reload_surface(UserlandHleCall& call, std::uint32_t surface) {
    const auto found = surfaces_.find(surface);
    if (found == surfaces_.end())
        return false;
    auto& state = found->second;
    if (!state.backing_identifier)
        return true;
    const auto backing = surface_store_->find(*state.backing_identifier);
    const auto host_surface =
        surface_store_->host_surface(*state.backing_identifier);
    if (backing && host_surface &&
        host_surface->gpu_generation() >
            host_surface->cpu_generation()) {
        state.width = backing->width;
        state.height = backing->height;
        state.refreshed_textures.clear();
        state.dirty = false;
        return true;
    }
    const auto pixels =
        surface_store_->read_argb(call.memory(), *state.backing_identifier);
    if (!backing || !pixels)
        return false;
    state.width = backing->width;
    state.height = backing->height;
    state.pixels = std::move(*pixels);
    state.refreshed_textures.clear();
    state.dirty = false;
    renderer_->invalidate(render_target_key(surface));
    return true;
}

bool OpenGlesHle::flush_surface(UserlandHleCall& call, std::uint32_t surface) {
    const auto found = surfaces_.find(surface);
    if (found == surfaces_.end())
        return false;
    auto& state = found->second;
    auto frame = state.backing_identifier
                     ? DisplayFrame{state.width, state.height, 0, state.pixels}
                 : display_ ? display_->snapshot()
                            : DisplayFrame{};
    if (frame.width == 0 || frame.height == 0 ||
        !renderer_->synchronize(frame, render_target_key(surface))) {
        return false;
    }
    if (!state.backing_identifier) {
        if (display_ && display_write_allowed(call))
            display_->replace_pixels(std::move(frame.pixels),
                                     call.process_id());
        state.refreshed_textures.clear();
        return true;
    }
    state.pixels = std::move(frame.pixels);
    state.dirty = true;
    if (!surface_store_->write_argb(call.memory(), *state.backing_identifier,
                                    state.pixels)) {
        return false;
    }
    state.dirty = false;
    state.refreshed_textures.clear();
    return true;
}

std::optional<DisplayFrame> OpenGlesHle::render_target(UserlandHleCall& call) {
    if (const auto* surface = current_pixmap_surface(call)) {
        if (surface->width == 0 || surface->height == 0 ||
            surface->pixels.size() !=
                static_cast<std::size_t>(surface->width) * surface->height) {
            return std::nullopt;
        }
        return DisplayFrame{surface->width, surface->height, 0,
                            surface->pixels};
    }
    return display_ ? std::optional<DisplayFrame>{display_->snapshot()}
                    : std::nullopt;
}

bool OpenGlesHle::commit_render_target(UserlandHleCall& call,
                                       DisplayFrame frame) {
    if (auto* surface = current_pixmap_surface(call)) {
        if (frame.width != surface->width || frame.height != surface->height ||
            frame.pixels.size() !=
                static_cast<std::size_t>(surface->width) * surface->height) {
            return false;
        }
        surface->pixels = std::move(frame.pixels);
        surface->dirty = true;
        return true;
    }
    if (!display_)
        return false;
    if (display_write_allowed(call)) {
        display_->replace_pixels(std::move(frame.pixels),
                                 call.process_id());
    }
    return true;
}

void OpenGlesHle::draw(UserlandHleCall& call, bool indexed) {
    auto* context = current_context(call);
    if (context == nullptr) {
        set_gl_error(call, gles_abi::invalid_operation);
        return;
    }
    const auto mode = call.argument(0);
    if (mode != gles_abi::triangles && mode != gles_abi::triangle_strip &&
        mode != gles_abi::triangle_fan) {
        set_gl_error(call, gles_abi::invalid_enum);
        return;
    }
    const auto first =
        indexed ? 0 : static_cast<std::int32_t>(call.argument(1));
    const auto count =
        static_cast<std::int32_t>(call.argument(indexed ? 1U : 2U));
    if (first < 0 || count < 0) {
        set_gl_error(call, gles_abi::invalid_value);
        return;
    }
    if (count == 0)
        return;
    if (static_cast<std::uint32_t>(count) > gles_abi::maximum_draw_vertices) {
        set_gl_error(call, gles_abi::out_of_memory);
        return;
    }
    std::vector<GlesRasterVertex> vertices;
    vertices.reserve(static_cast<std::size_t>(count));
    const auto index_type = indexed ? call.argument(2) : 0;
    if (indexed && index_type != gles_abi::unsigned_byte &&
        index_type != gles_abi::unsigned_short) {
        set_gl_error(call, gles_abi::invalid_enum);
        return;
    }
    const auto index_size = index_type == gles_abi::unsigned_short ? 2U : 1U;
    const auto index_pointer = indexed ? call.argument(3) : 0;
    const auto* element_buffer =
        indexed && context->bound_element_array_buffer != 0
            ? resources_.buffer(context->bound_element_array_buffer)
            : nullptr;
    for (std::uint32_t item = 0; item < static_cast<std::uint32_t>(count);
         ++item) {
        auto vertex_index = static_cast<std::uint32_t>(first) + item;
        if (indexed) {
            const auto offset = static_cast<std::uint64_t>(index_pointer) +
                                static_cast<std::uint64_t>(item) * index_size;
            if (element_buffer != nullptr) {
                if (offset > element_buffer->bytes.size() ||
                    index_size > element_buffer->bytes.size() - offset) {
                    set_gl_error(call, gles_abi::invalid_operation);
                    return;
                }
                vertex_index = std::to_integer<std::uint32_t>(
                    element_buffer->bytes[static_cast<std::size_t>(offset)]);
                if (index_size == 2) {
                    vertex_index |=
                        std::to_integer<std::uint32_t>(
                            element_buffer
                                ->bytes[static_cast<std::size_t>(offset) + 1U])
                        << 8U;
                }
            } else {
                if (offset > std::numeric_limits<std::uint32_t>::max()) {
                    set_gl_error(call, gles_abi::invalid_operation);
                    return;
                }
                if (index_size == 2) {
                    const auto value = call.memory().read16(
                        static_cast<std::uint32_t>(offset));
                    if (!value) {
                        set_gl_error(call, gles_abi::invalid_operation);
                        return;
                    }
                    vertex_index = *value;
                } else {
                    const auto value =
                        call.memory().read8(static_cast<std::uint32_t>(offset));
                    if (!value) {
                        set_gl_error(call, gles_abi::invalid_operation);
                        return;
                    }
                    vertex_index = *value;
                }
            }
        }
        const auto vertex = read_vertex(call, *context, vertex_index);
        if (!vertex) {
            set_gl_error(call, gles_abi::invalid_operation);
            return;
        }
        vertices.push_back(*vertex);
    }
    if (vertices.size() < 3)
        return;
    auto target = render_target(call);
    if (!target) {
        set_gl_error(call, gles_abi::invalid_operation);
        return;
    }
    const auto viewport_width =
        context->viewport[2] > 0
            ? static_cast<std::uint32_t>(context->viewport[2])
            : target->width;
    const auto viewport_height =
        context->viewport[3] > 0
            ? static_cast<std::uint32_t>(context->viewport[3])
            : target->height;
    GlesRasterState state;
    state.resource_owner = renderer_owner_;
    state.viewport_x = context->viewport[0];
    state.viewport_y = context->viewport[1];
    state.viewport_width = viewport_width;
    state.viewport_height = viewport_height;
    state.resources = &resources_;
    state.blend_enabled =
        context->enabled_capabilities.contains(gles_abi::blend);
    state.cull_enabled =
        context->enabled_capabilities.contains(gles_abi::cull_face);
    state.scissor_enabled =
        context->enabled_capabilities.contains(gles_abi::scissor_test);
    state.scissor_box = context->scissor_box;
    state.color_mask = context->color_mask;
    state.blend_source = context->blend_source;
    state.blend_destination = context->blend_destination;
    state.cull_mode = context->cull_mode;
    state.front_face = context->front_face;
    auto* pixmap_surface = current_pixmap_surface(call);
    for (std::size_t unit_index = 0; unit_index < context->texture_units.size();
         ++unit_index) {
        const auto& unit = context->texture_units[unit_index];
        const auto rectangle_enabled = unit.texture_rectangle_enabled;
        auto& raster_unit = state.texture_units[unit_index];
        raster_unit.enabled = rectangle_enabled || unit.texture_2d_enabled;
        raster_unit.rectangle = rectangle_enabled;
        raster_unit.environment = unit.texture_environment;
        raster_unit.texture = rectangle_enabled ? unit.bound_texture_rectangle
                                                : unit.bound_texture_2d;
        if (!raster_unit.enabled)
            continue;
        if (pixmap_surface == nullptr ||
            !pixmap_surface->refreshed_textures.contains(
                raster_unit.texture)) {
            const auto refresh_error = resources_.refresh_surface_texture(
                call.memory(), raster_unit.texture, *surface_store_);
            if (refresh_error != gles_abi::no_error) {
                set_gl_error(call, refresh_error);
                return;
            }
            if (pixmap_surface != nullptr)
                pixmap_surface->refreshed_textures.insert(raster_unit.texture);
        }
    }
    const auto target_key = render_target_key(thread(call).draw_surface);
    const auto pixmap_target = pixmap_surface != nullptr;
    if (!pixmap_target) {
        renderer_->invalidate(target_key);
    }
    performance_counters().record_draw();
    auto rendered = renderer_->draw(*target, target_key, vertices, mode, state);
    const auto native_rendered =
        rendered && pixmap_target && renderer_->accelerated() &&
        renderer_->failure_reason() == PerfFallbackReason::None;
    if (native_rendered) {
        const auto identifier = pixmap_surface->backing_identifier;
        if (identifier) {
            if (const auto surface =
                    surface_store_->host_surface(*identifier)) {
                static_cast<void>(surface->mark_gpu_write());
            }
        }
    } else if (rendered && pixmap_target) {
        const auto identifier = pixmap_surface->backing_identifier;
        if (identifier &&
            !surface_store_->write_argb(
                call.memory(), *identifier, target->pixels)) {
                rendered = false;
        }
    }
    if (rendered && !pixmap_target) {
        rendered = renderer_->synchronize(*target, target_key);
    }
    if (!rendered || !commit_render_target(call, std::move(*target))) {
        set_gl_error(call, gles_abi::invalid_operation);
    }
}

void OpenGlesHle::register_egl(UserlandHleRegistry& registry) {
    const auto add = [&](std::string symbol,
                         UserlandHleRegistry::Handler handler) {
        registry.register_function(std::string{opengles_image},
                                   std::move(symbol), std::move(handler));
    };
    add("_eglGetDisplay", [this](UserlandHleCall& call) {
        egl_error_ = egl_success;
        call.set_return(egl_default_display);
    });
    add("_eglInitialize", [this](UserlandHleCall& call) {
        if (!is_valid_display(call.argument(0))) {
            egl_error_ = egl_bad_display;
            call.set_return(egl_false);
            return;
        }
        if (!call.write32(call.argument(1), 1) ||
            !call.write32(call.argument(2), 1)) {
            egl_error_ = egl_bad_parameter;
            call.set_return(egl_false);
            return;
        }
        egl_error_ = egl_success;
        call.set_return(egl_true);
    });
    add("_eglTerminate", [this](UserlandHleCall& call) {
        if (!is_valid_display(call.argument(0))) {
            egl_error_ = egl_bad_display;
            call.set_return(egl_false);
            return;
        }
        egl_error_ = egl_success;
        call.set_return(egl_true);
    });
    add("_eglGetError", [this](UserlandHleCall& call) {
        call.set_return(egl_error_);
        egl_error_ = egl_success;
    });
    const auto enumerate_configs = [this](UserlandHleCall& call) {
        if (!is_valid_display(call.argument(0))) {
            egl_error_ = egl_bad_display;
            call.set_return(egl_false);
            return;
        }
        // eglChooseConfig has its count pointer in argument 4; eglGetConfigs
        // has it in argument 3.
        const auto choose = call.symbol() == "_eglChooseConfig";
        const auto configs = call.argument(choose ? 2U : 1U);
        const auto capacity = call.argument(choose ? 3U : 2U);
        const auto count = call.argument(choose ? 4U : 3U);
        if (!call.write32(count, 1) ||
            (configs != 0 && capacity != 0 &&
             !call.memory().write32(configs, egl_default_config))) {
            egl_error_ = egl_bad_parameter;
            call.set_return(egl_false);
            return;
        }
        egl_error_ = egl_success;
        call.set_return(egl_true);
    };
    add("_eglGetConfigs", enumerate_configs);
    add("_eglChooseConfig", enumerate_configs);
    add("_eglGetConfigAttrib", [this](UserlandHleCall& call) {
        if (!is_valid_display(call.argument(0))) {
            egl_error_ = egl_bad_display;
            call.set_return(egl_false);
            return;
        }
        if (call.argument(1) != egl_default_config) {
            egl_error_ = egl_bad_parameter;
            call.set_return(egl_false);
            return;
        }
        std::uint32_t value = 0;
        switch (call.argument(2)) {
        case egl_buffer_size: value = 32; break;
        case egl_alpha_size:
        case egl_blue_size:
        case egl_green_size:
        case egl_red_size: value = 8; break;
        case egl_depth_size: value = 24; break;
        case egl_stencil_size: value = 8; break;
        case egl_config_id: value = egl_default_config; break;
        case egl_surface_type: value = egl_window_bit | egl_pbuffer_bit; break;
        case egl_renderable_type: value = egl_opengl_es_bit; break;
        default: value = 0; break;
        }
        if (!call.write32(call.argument(3), value)) {
            egl_error_ = egl_bad_parameter;
            call.set_return(egl_false);
            return;
        }
        egl_error_ = egl_success;
        call.set_return(egl_true);
    });
    add("_eglCreateContext", [this](UserlandHleCall& call) {
        if (!is_valid_display(call.argument(0))) {
            egl_error_ = egl_bad_display;
            call.set_return(0);
            return;
        }
        const auto context = next_context_++;
        auto state = ContextState{};
        const auto geometry =
            display_ ? display_->geometry() : default_display_geometry;
        state.viewport = {0, 0, static_cast<std::int32_t>(geometry.width),
                          static_cast<std::int32_t>(geometry.height)};
        state.scissor_box = state.viewport;
        contexts_.emplace(context, std::move(state));
        egl_error_ = egl_success;
        call.set_return(context);
    });
    add("_eglDestroyContext", [this](UserlandHleCall& call) {
        if (!is_valid_display(call.argument(0))) {
            egl_error_ = egl_bad_display;
            call.set_return(egl_false);
            return;
        }
        const auto destroyed = call.argument(1);
        if (contexts_.erase(destroyed) == 0) {
            egl_error_ = egl_bad_context;
            call.set_return(egl_false);
            return;
        }
        for (auto& [processor, current] : threads_) {
            static_cast<void>(processor);
            if (current.context == destroyed)
                current = {};
        }
        egl_error_ = egl_success;
        call.set_return(egl_true);
    });
    const auto create_surface = [this](UserlandHleCall& call) {
        if (!is_valid_display(call.argument(0))) {
            egl_error_ = egl_bad_display;
            call.set_return(0);
            return;
        }
        SurfaceState state;
        const auto geometry =
            display_ ? display_->geometry() : default_display_geometry;
        state.width = geometry.width;
        state.height = geometry.height;
        if (call.symbol() == "_eglCreatePixmapSurface") {
            const auto identifier =
                core_surface_identifier(call, call.argument(2));
            const auto backing =
                identifier ? surface_store_->find(*identifier) : std::nullopt;
            const auto pixels =
                identifier
                    ? surface_store_->read_argb(call.memory(), *identifier)
                    : std::nullopt;
            if (!identifier || !backing || !pixels) {
                egl_error_ = egl_bad_parameter;
                call.set_return(0);
                return;
            }
            state.backing_identifier = *identifier;
            state.width = backing->width;
            state.height = backing->height;
            state.pixels = std::move(*pixels);
        }
        const auto surface = next_surface_++;
        surfaces_.emplace(surface, std::move(state));
        egl_error_ = egl_success;
        call.set_return(surface);
    };
    add("_eglCreateWindowSurface", create_surface);
    add("_eglCreatePbufferSurface", create_surface);
    add("_eglCreatePixmapSurface", create_surface);
    add("_eglDestroySurface", [this](UserlandHleCall& call) {
        if (!is_valid_display(call.argument(0))) {
            egl_error_ = egl_bad_display;
            call.set_return(egl_false);
            return;
        }
        const auto surface = call.argument(1);
        if (!surfaces_.contains(surface)) {
            egl_error_ = egl_bad_surface;
            call.set_return(egl_false);
            return;
        }
        const auto& state = surfaces_.at(surface);
        const auto committed =
            state.backing_identifier
                ? renderer_->flush(render_target_key(surface))
                : flush_surface(call, surface);
        if (!committed) {
            egl_error_ = egl_bad_surface;
            call.set_return(egl_false);
            return;
        }
        if (!surfaces_.at(surface).backing_identifier) {
            const auto target = render_target_key(surface);
            renderer_->release(std::span{&target, 1U});
        }
        surfaces_.erase(surface);
        for (auto& [processor, current] : threads_) {
            static_cast<void>(processor);
            if (current.draw_surface == surface)
                current.draw_surface = 0;
            if (current.read_surface == surface)
                current.read_surface = 0;
        }
        egl_error_ = egl_success;
        call.set_return(egl_true);
    });
    add("_eglMakeCurrent", [this](UserlandHleCall& call) {
        if (!is_valid_display(call.argument(0))) {
            egl_error_ = egl_bad_display;
            call.set_return(egl_false);
            return;
        }
        const auto draw = call.argument(1);
        const auto read = call.argument(2);
        const auto context = call.argument(3);
        if ((context != 0 && !contexts_.contains(context)) ||
            (draw != 0 && !surfaces_.contains(draw)) ||
            (read != 0 && !surfaces_.contains(read))) {
            egl_error_ = context != 0 && !contexts_.contains(context)
                             ? egl_bad_context
                             : egl_bad_surface;
            call.set_return(egl_false);
            return;
        }
        auto& current = thread(call);
        if (current.draw_surface != 0 && current.draw_surface != draw) {
            const auto previous = surfaces_.find(current.draw_surface);
            const auto committed =
                previous != surfaces_.end() &&
                        previous->second.backing_identifier
                    ? renderer_->flush(
                          render_target_key(current.draw_surface))
                    : flush_surface(call, current.draw_surface);
            if (!committed) {
                egl_error_ = egl_bad_surface;
                call.set_return(egl_false);
                return;
            }
        }
        if (draw != 0 && current.draw_surface != draw &&
            !reload_surface(call, draw)) {
            egl_error_ = egl_bad_surface;
            call.set_return(egl_false);
            return;
        }
        current.display = context == 0 ? 0 : egl_default_display;
        current.draw_surface = draw;
        current.read_surface = read;
        current.context = context;
        egl_error_ = egl_success;
        call.set_return(egl_true);
    });
    add("_eglGetCurrentContext", [this](UserlandHleCall& call) {
        call.set_return(thread(call).context);
    });
    add("_eglGetCurrentDisplay", [this](UserlandHleCall& call) {
        call.set_return(thread(call).display);
    });
    add("_eglGetCurrentSurface", [this](UserlandHleCall& call) {
        const auto& current = thread(call);
        call.set_return(call.argument(0) == 0x305aU ? current.read_surface
                                                    : current.draw_surface);
    });
    add("_eglSwapBuffers", [this](UserlandHleCall& call) {
        if (!is_valid_display(call.argument(0)) ||
            !surfaces_.contains(call.argument(1))) {
            egl_error_ = !is_valid_display(call.argument(0)) ? egl_bad_display
                                                             : egl_bad_surface;
            call.set_return(egl_false);
            return;
        }
        const auto surface = call.argument(1);
        const auto& state = surfaces_.at(surface);
        const auto committed =
            state.backing_identifier
                ? renderer_->flush(render_target_key(surface))
                : flush_surface(call, surface);
        if (!committed) {
            egl_error_ = egl_bad_surface;
            call.set_return(egl_false);
            return;
        }
        ++frame_count_;
        const auto found = surfaces_.find(surface);
        if (found != surfaces_.end() && !found->second.backing_identifier &&
            display_ && display_write_allowed(call)) {
            display_->present(call.process_id());
        }
        egl_error_ = egl_success;
        call.set_return(egl_true);
    });
    add("_eglSwapInterval", [this](UserlandHleCall& call) {
        if (!is_valid_display(call.argument(0))) {
            egl_error_ = egl_bad_display;
            call.set_return(egl_false);
            return;
        }
        egl_error_ = egl_success;
        call.set_return(egl_true);
    });
    add("_eglQueryString", [this](UserlandHleCall& call) {
        if (!is_valid_display(call.argument(0))) {
            egl_error_ = egl_bad_display;
            call.set_return(0);
            return;
        }
        std::string_view value;
        switch (call.argument(1)) {
        case egl_vendor: value = "iLEmu"; break;
        case egl_version: value = "1.1 iLEmu userland HLE"; break;
        case egl_extensions: value = ""; break;
        default:
            egl_error_ = egl_bad_parameter;
            call.set_return(0);
            return;
        }
        egl_error_ = egl_success;
        call.set_return(call.intern_string(value));
    });
    const auto query_object = [this](UserlandHleCall& call) {
        if (!is_valid_display(call.argument(0))) {
            egl_error_ = egl_bad_display;
            call.set_return(egl_false);
            return;
        }
        std::uint32_t value = 0;
        if (call.symbol() == "_eglQuerySurface") {
            const auto surface = surfaces_.find(call.argument(1));
            if (surface == surfaces_.end()) {
                egl_error_ = egl_bad_surface;
                call.set_return(egl_false);
                return;
            }
            if (call.argument(2) == egl_width) {
                value = surface->second.width;
            }
            if (call.argument(2) == egl_height) {
                value = surface->second.height;
            }
        } else {
            if (!contexts_.contains(call.argument(1))) {
                egl_error_ = egl_bad_context;
                call.set_return(egl_false);
                return;
            }
            if (call.argument(2) == egl_context_client_version)
                value = 1;
        }
        if (!call.write32(call.argument(3), value)) {
            egl_error_ = egl_bad_parameter;
            call.set_return(egl_false);
            return;
        }
        egl_error_ = egl_success;
        call.set_return(egl_true);
    };
    add("_eglQuerySurface", query_object);
    add("_eglQueryContext", query_object);
    add("_eglGetProcAddress", [](UserlandHleCall& call) {
        const auto name = call.string_argument(0, 256);
        call.set_return(name ? call.symbol_address("_" + *name).value_or(0)
                             : 0);
    });
    const auto success = [this](UserlandHleCall& call) {
        egl_error_ = egl_success;
        call.set_return(egl_true);
    };
    add("_eglWaitGL", success);
    add("_eglWaitNative", success);
    add("_eglCopyBuffers", success);
    add("_eglBindTexImage", success);
    add("_eglReleaseTexImage", success);
    add("_eglSurfaceAttrib", success);
    add("_eglSwapNotification", success);
    registry.register_prefix(
        std::string{opengles_image}, "_egl",
        [this](UserlandHleCall& call) { unsupported(call); });
}

void OpenGlesHle::register_gles(UserlandHleRegistry& registry) {
    const auto add = [&](std::string symbol,
                         UserlandHleRegistry::Handler handler) {
        registry.register_function(std::string{opengles_image},
                                   std::move(symbol), std::move(handler));
    };
    add("_glGetError", [this](UserlandHleCall& call) {
        auto& current = thread(call);
        call.set_return(current.gl_error);
        current.gl_error = gl_no_error;
    });
    add("_glGetString", [this](UserlandHleCall& call) {
        std::string_view value;
        switch (call.argument(0)) {
        case gl_vendor: value = "iLEmu"; break;
        case gl_renderer: value = renderer_->name(); break;
        case gl_version: value = "OpenGL ES-CM 1.1 iLEmu"; break;
        case gl_extensions:
            value = "GL_OES_fixed_point GL_OES_single_precision "
                    "GL_APPLE_texture_rectangle ";
            break;
        default: call.set_return(0); return;
        }
        call.set_return(call.intern_string(value));
    });
    add("_glActiveTexture", [this](UserlandHleCall& call) {
        auto* context = current_context(call);
        const auto texture = call.argument(0);
        const auto index = texture >= gles_abi::texture0
                               ? texture - gles_abi::texture0
                               : std::numeric_limits<std::uint32_t>::max();
        if (context == nullptr) {
            set_gl_error(call, gles_abi::invalid_operation);
        } else if (index >= gles_abi::texture_unit_count) {
            set_gl_error(call, gles_abi::invalid_enum);
        } else {
            context->active_texture_unit = index;
        }
    });
    add("_glClientActiveTexture", [this](UserlandHleCall& call) {
        auto* context = current_context(call);
        const auto texture = call.argument(0);
        const auto index = texture >= gles_abi::texture0
                               ? texture - gles_abi::texture0
                               : std::numeric_limits<std::uint32_t>::max();
        if (context == nullptr) {
            set_gl_error(call, gles_abi::invalid_operation);
        } else if (index >= gles_abi::texture_unit_count) {
            set_gl_error(call, gles_abi::invalid_enum);
        } else {
            context->client_active_texture_unit = index;
        }
    });
    add("_glColor4f", [this](UserlandHleCall& call) {
        auto* context = current_context(call);
        if (context == nullptr) {
            set_gl_error(call, gles_abi::invalid_operation);
            return;
        }
        for (std::size_t component = 0;
             component < context->current_color.size(); ++component) {
            context->current_color[component] =
                std::bit_cast<float>(call.argument(component));
        }
    });
    add("_glColor4ub", [this](UserlandHleCall& call) {
        auto* context = current_context(call);
        if (context == nullptr) {
            set_gl_error(call, gles_abi::invalid_operation);
            return;
        }
        for (std::size_t component = 0;
             component < context->current_color.size(); ++component) {
            context->current_color[component] =
                static_cast<float>(call.argument(component) & 0xffU) / 255.0F;
        }
    });
    add("_glGetIntegerv", [this](UserlandHleCall& call) {
        auto* context = current_context(call);
        const auto output = call.argument(1);
        if (context == nullptr || output == 0) {
            set_gl_error(call, context == nullptr ? gles_abi::invalid_operation
                                                  : gles_abi::invalid_value);
            return;
        }
        std::array<std::uint32_t, 4> values{};
        std::size_t count = 1;
        switch (call.argument(0)) {
        case gles_abi::viewport_query:
            count = context->viewport.size();
            for (std::size_t index = 0; index < count; ++index) {
                values[index] =
                    static_cast<std::uint32_t>(context->viewport[index]);
            }
            break;
        case gles_abi::scissor_box:
            count = context->scissor_box.size();
            for (std::size_t index = 0; index < count; ++index) {
                values[index] =
                    static_cast<std::uint32_t>(context->scissor_box[index]);
            }
            break;
        case gles_abi::color_write_mask:
            count = context->color_mask.size();
            for (std::size_t index = 0; index < count; ++index) {
                values[index] = context->color_mask[index] ? 1U : 0U;
            }
            break;
        case gles_abi::matrix_mode_query:
            values[0] = context->matrix_mode;
            break;
        case gles_abi::texture_binding_2d:
            values[0] = context->texture_units[context->active_texture_unit]
                            .bound_texture_2d;
            break;
        case gles_abi::texture_binding_rectangle_apple:
            values[0] = context->texture_units[context->active_texture_unit]
                            .bound_texture_rectangle;
            break;
        case gles_abi::active_texture:
            values[0] = gles_abi::texture0 + static_cast<std::uint32_t>(
                                                 context->active_texture_unit);
            break;
        case gles_abi::client_active_texture:
            values[0] =
                gles_abi::texture0 +
                static_cast<std::uint32_t>(context->client_active_texture_unit);
            break;
        case gles_abi::maximum_texture_units:
            values[0] =
                static_cast<std::uint32_t>(gles_abi::texture_unit_count);
            break;
        case gles_abi::maximum_texture_size:
            values[0] = gles_abi::maximum_texture_dimension;
            break;
        case gles_abi::front_face_query: values[0] = context->front_face; break;
        case gles_abi::cull_face_mode: values[0] = context->cull_mode; break;
        case gles_abi::depth_write_mask:
            values[0] = context->depth_mask ? 1U : 0U;
            break;
        case gles_abi::stencil_write_mask:
            values[0] = context->stencil_mask;
            break;
        default: set_gl_error(call, gles_abi::invalid_enum); return;
        }
        for (std::size_t index = 0; index < count; ++index) {
            if (!call.memory().write32(
                    output + static_cast<std::uint32_t>(index * 4U),
                    values[index])) {
                set_gl_error(call, gles_abi::invalid_value);
                return;
            }
        }
    });
    add("_glGetFloatv", [this](UserlandHleCall& call) {
        auto* context = current_context(call);
        const auto output = call.argument(1);
        if (context == nullptr || output == 0) {
            set_gl_error(call, context == nullptr ? gles_abi::invalid_operation
                                                  : gles_abi::invalid_value);
            return;
        }
        const std::array<float, 16>* values{};
        std::array<float, 16> current{};
        std::size_t count{};
        if (call.argument(0) == gles_abi::current_color) {
            std::copy(context->current_color.begin(),
                      context->current_color.end(), current.begin());
            values = &current;
            count = context->current_color.size();
        } else if (call.argument(0) == gles_abi::modelview_matrix_query) {
            values = &context->modelview_matrix.values();
            count = values->size();
        } else if (call.argument(0) == gles_abi::projection_matrix_query) {
            values = &context->projection_matrix.values();
            count = values->size();
        } else if (call.argument(0) == gles_abi::texture_matrix_query) {
            values = &context->texture_units[context->active_texture_unit]
                          .texture_matrix.values();
            count = values->size();
        } else {
            set_gl_error(call, gles_abi::invalid_enum);
            return;
        }
        for (std::size_t index = 0; index < count; ++index) {
            if (!call.memory().write32(
                    output + static_cast<std::uint32_t>(index * 4U),
                    std::bit_cast<std::uint32_t>((*values)[index]))) {
                set_gl_error(call, gles_abi::invalid_value);
                return;
            }
        }
    });
    add("_glMatrixMode", [this](UserlandHleCall& call) {
        auto* context = current_context(call);
        const auto mode = call.argument(0);
        if (context == nullptr) {
            set_gl_error(call, gles_abi::invalid_operation);
        } else if (mode != gles_abi::modelview &&
                   mode != gles_abi::projection &&
                   mode != gles_abi::texture_matrix) {
            set_gl_error(call, gles_abi::invalid_enum);
        } else {
            context->matrix_mode = mode;
        }
    });
    add("_glLoadIdentity", [this](UserlandHleCall& call) {
        auto* context = current_context(call);
        auto* matrix = context == nullptr ? nullptr : current_matrix(*context);
        if (matrix == nullptr) {
            set_gl_error(call, gles_abi::invalid_operation);
        } else {
            *matrix = GlesMatrix{};
        }
    });
    const auto matrix_data = [this](UserlandHleCall& call) {
        auto* context = current_context(call);
        auto* current = context == nullptr ? nullptr : current_matrix(*context);
        const auto address = call.argument(0);
        if (current == nullptr || address == 0) {
            set_gl_error(call, current == nullptr ? gles_abi::invalid_operation
                                                  : gles_abi::invalid_value);
            return;
        }
        const auto fixed = call.symbol() == "_glLoadMatrixx" ||
                           call.symbol() == "_glMultMatrixx";
        std::array<float, 16> values{};
        for (std::size_t index = 0; index < values.size(); ++index) {
            const auto raw = call.memory().read32(
                address + static_cast<std::uint32_t>(index * 4U));
            if (!raw) {
                set_gl_error(call, gles_abi::invalid_value);
                return;
            }
            values[index] =
                fixed ? static_cast<float>(static_cast<std::int32_t>(*raw)) /
                            65'536.0F
                      : std::bit_cast<float>(*raw);
        }
        const GlesMatrix matrix{values};
        if (call.symbol() == "_glLoadMatrixf" ||
            call.symbol() == "_glLoadMatrixx") {
            *current = matrix;
        } else {
            *current = *current * matrix;
        }
    };
    add("_glLoadMatrixf", matrix_data);
    add("_glLoadMatrixx", matrix_data);
    add("_glMultMatrixf", matrix_data);
    add("_glMultMatrixx", matrix_data);
    const auto transform = [this](UserlandHleCall& call) {
        const auto fixed = call.symbol() == "_glTranslatex" ||
                           call.symbol() == "_glScalex" ||
                           call.symbol() == "_glRotatex";
        const auto value = [&](std::size_t index) {
            const auto raw = call.argument(index);
            return fixed ? static_cast<float>(static_cast<std::int32_t>(raw)) /
                               65'536.0F
                         : std::bit_cast<float>(raw);
        };
        if (call.symbol() == "_glTranslatef" ||
            call.symbol() == "_glTranslatex") {
            multiply_current_matrix(
                call, GlesMatrix::translation(value(0), value(1), value(2)));
        } else if (call.symbol() == "_glScalef" ||
                   call.symbol() == "_glScalex") {
            multiply_current_matrix(
                call, GlesMatrix::scale(value(0), value(1), value(2)));
        } else {
            multiply_current_matrix(
                call,
                GlesMatrix::rotation(value(0), value(1), value(2), value(3)));
        }
    };
    add("_glTranslatef", transform);
    add("_glTranslatex", transform);
    add("_glScalef", transform);
    add("_glScalex", transform);
    add("_glRotatef", transform);
    add("_glRotatex", transform);
    const auto projection = [this](UserlandHleCall& call) {
        const auto fixed =
            call.symbol() == "_glOrthox" || call.symbol() == "_glFrustumx";
        const auto value = [&](std::size_t index) {
            const auto raw = call.argument(index);
            return fixed ? static_cast<float>(static_cast<std::int32_t>(raw)) /
                               65'536.0F
                         : std::bit_cast<float>(raw);
        };
        const auto left = value(0);
        const auto right = value(1);
        const auto bottom = value(2);
        const auto top = value(3);
        const auto near_value = value(4);
        const auto far_value = value(5);
        const auto frustum =
            call.symbol() == "_glFrustumf" || call.symbol() == "_glFrustumx";
        if (left == right || bottom == top || near_value == far_value ||
            (frustum && (near_value <= 0.0F || far_value <= 0.0F))) {
            set_gl_error(call, gles_abi::invalid_value);
            return;
        }
        multiply_current_matrix(
            call, frustum ? GlesMatrix::frustum(left, right, bottom, top,
                                                near_value, far_value)
                          : GlesMatrix::orthographic(left, right, bottom, top,
                                                     near_value, far_value));
    };
    add("_glOrthof", projection);
    add("_glOrthox", projection);
    add("_glFrustumf", projection);
    add("_glFrustumx", projection);
    add("_glPushMatrix", [this](UserlandHleCall& call) {
        auto* context = current_context(call);
        auto* matrix = context == nullptr ? nullptr : current_matrix(*context);
        auto* stack =
            context == nullptr ? nullptr : current_matrix_stack(*context);
        if (matrix == nullptr || stack == nullptr ||
            stack->size() >= gles_abi::maximum_matrix_stack_depth) {
            set_gl_error(call, gles_abi::invalid_operation);
            return;
        }
        stack->push_back(*matrix);
    });
    add("_glPopMatrix", [this](UserlandHleCall& call) {
        auto* context = current_context(call);
        auto* matrix = context == nullptr ? nullptr : current_matrix(*context);
        auto* stack =
            context == nullptr ? nullptr : current_matrix_stack(*context);
        if (matrix == nullptr || stack == nullptr || stack->empty()) {
            set_gl_error(call, gles_abi::invalid_operation);
            return;
        }
        *matrix = stack->back();
        stack->pop_back();
    });
    add("_glClearColor", [this](UserlandHleCall& call) {
        auto* context = current_context(call);
        if (context == nullptr) {
            set_gl_error(call, gles_abi::invalid_operation);
            return;
        }
        for (std::size_t index = 0; index < 4; ++index) {
            context->clear_color[index] = call.argument(index);
        }
        const auto channel = [](std::uint32_t bits) {
            const auto value =
                std::clamp(std::bit_cast<float>(bits), 0.0F, 1.0F);
            return static_cast<std::uint32_t>(std::lround(value * 255.0F));
        };
        const auto red = channel(context->clear_color[0]);
        const auto green = channel(context->clear_color[1]);
        const auto blue = channel(context->clear_color[2]);
        const auto alpha = channel(context->clear_color[3]);
        context->clear_argb =
            (alpha << 24U) | (red << 16U) | (green << 8U) | blue;
    });
    add("_glClearColorx", [this](UserlandHleCall& call) {
        auto* context = current_context(call);
        if (context == nullptr) {
            set_gl_error(call, gles_abi::invalid_operation);
            return;
        }
        for (std::size_t index = 0; index < 4; ++index) {
            context->clear_color[index] = call.argument(index);
        }
        const auto channel = [](std::uint32_t fixed) {
            const auto signed_value = static_cast<std::int32_t>(fixed);
            return static_cast<std::uint32_t>(
                std::clamp(signed_value, 0, 65'536) * 255 / 65'536);
        };
        const auto red = channel(context->clear_color[0]);
        const auto green = channel(context->clear_color[1]);
        const auto blue = channel(context->clear_color[2]);
        const auto alpha = channel(context->clear_color[3]);
        context->clear_argb =
            (alpha << 24U) | (red << 16U) | (green << 8U) | blue;
    });
    add("_glClear", [this](UserlandHleCall& call) {
        auto* context = current_context(call);
        if (context == nullptr) {
            set_gl_error(call, gles_abi::invalid_operation);
            return;
        }
        constexpr auto supported_bits = gles_abi::color_buffer_bit |
                                        gles_abi::depth_buffer_bit |
                                        gles_abi::stencil_buffer_bit;
        const auto mask = call.argument(0);
        if ((mask & ~supported_bits) != 0) {
            set_gl_error(call, gles_abi::invalid_value);
            return;
        }
        if ((mask & gles_abi::color_buffer_bit) == 0)
            return;
        auto frame = render_target(call);
        if (!frame) {
            set_gl_error(call, gles_abi::invalid_operation);
            return;
        }
        const auto target = render_target_key(thread(call).draw_surface);
        const auto all_channels =
            std::all_of(context->color_mask.begin(), context->color_mask.end(),
                        [](bool enabled) { return enabled; });
        const auto scissor_enabled =
            context->enabled_capabilities.contains(gles_abi::scissor_test);
        if (all_channels && renderer_->accelerated() && command_encoder_) {
            const auto* pixmap = current_pixmap_surface(call);
            const auto identifier =
                pixmap ? pixmap->backing_identifier : std::nullopt;
            const auto surface =
                identifier ? surface_store_->host_surface(*identifier)
                           : nullptr;
            if (surface) {
                const auto width = static_cast<std::int64_t>(frame->width);
                const auto height = static_cast<std::int64_t>(frame->height);
                std::int64_t left{};
                std::int64_t top{};
                auto right = width;
                auto bottom = height;
                if (scissor_enabled) {
                    const auto requested_left =
                        static_cast<std::int64_t>(context->scissor_box[0]);
                    const auto requested_bottom =
                        static_cast<std::int64_t>(context->scissor_box[1]);
                    const auto requested_right =
                        requested_left + std::max<std::int64_t>(
                                             0, context->scissor_box[2]);
                    const auto requested_top =
                        requested_bottom + std::max<std::int64_t>(
                                               0, context->scissor_box[3]);
                    left = std::clamp(requested_left, std::int64_t{0}, width);
                    right =
                        std::clamp(requested_right, std::int64_t{0}, width);
                    top = height -
                          std::clamp(requested_top, std::int64_t{0}, height);
                    bottom =
                        height - std::clamp(requested_bottom,
                                            std::int64_t{0}, height);
                }
                if (right <= left || bottom <= top)
                    return;
                if (command_encoder_->fill(
                        surface,
                        {static_cast<std::int32_t>(left),
                         static_cast<std::int32_t>(top),
                         static_cast<std::uint32_t>(right - left),
                         static_cast<std::uint32_t>(bottom - top)},
                        context->clear_argb)) {
                    return;
                }
            }
        }
        if (!renderer_->synchronize(*frame, target)) {
            set_gl_error(call, gles_abi::invalid_operation);
            return;
        }
        if (all_channels && !scissor_enabled) {
            std::fill(frame->pixels.begin(), frame->pixels.end(),
                      context->clear_argb);
            if (!commit_render_target(call, std::move(*frame))) {
                set_gl_error(call, gles_abi::invalid_operation);
            } else {
                renderer_->invalidate(target);
            }
            return;
        }
        constexpr std::array<std::uint32_t, 4> channel_masks{
            0x00ff0000U, 0x0000ff00U, 0x000000ffU, 0xff000000U};
        for (std::uint32_t y = 0; y < frame->height; ++y) {
            const auto guest_y = static_cast<float>(frame->height) -
                                 (static_cast<float>(y) + 0.5F);
            for (std::uint32_t x = 0; x < frame->width; ++x) {
                const auto guest_x = static_cast<float>(x) + 0.5F;
                if (scissor_enabled &&
                    (guest_x < static_cast<float>(context->scissor_box[0]) ||
                     guest_x >=
                         static_cast<float>(context->scissor_box[0]) +
                             static_cast<float>(context->scissor_box[2]) ||
                     guest_y < static_cast<float>(context->scissor_box[1]) ||
                     guest_y >=
                         static_cast<float>(context->scissor_box[1]) +
                             static_cast<float>(context->scissor_box[3]))) {
                    continue;
                }
                const auto offset =
                    static_cast<std::size_t>(y) * frame->width + x;
                auto pixel = frame->pixels[offset];
                for (std::size_t component = 0;
                     component < context->color_mask.size(); ++component) {
                    if (!context->color_mask[component])
                        continue;
                    pixel = (pixel & ~channel_masks[component]) |
                            (context->clear_argb & channel_masks[component]);
                }
                frame->pixels[offset] = pixel;
            }
        }
        if (!commit_render_target(call, std::move(*frame))) {
            set_gl_error(call, gles_abi::invalid_operation);
        } else {
            renderer_->invalidate(target);
        }
    });
    add("_glViewport", [this](UserlandHleCall& call) {
        auto* context = current_context(call);
        if (context == nullptr) {
            set_gl_error(call, gles_abi::invalid_operation);
            return;
        }
        const auto width = static_cast<std::int32_t>(call.argument(2));
        const auto height = static_cast<std::int32_t>(call.argument(3));
        if (width < 0 || height < 0) {
            set_gl_error(call, gles_abi::invalid_value);
            return;
        }
        context->viewport = {static_cast<std::int32_t>(call.argument(0)),
                             static_cast<std::int32_t>(call.argument(1)), width,
                             height};
    });
    add("_glScissor", [this](UserlandHleCall& call) {
        auto* context = current_context(call);
        if (context == nullptr) {
            set_gl_error(call, gles_abi::invalid_operation);
            return;
        }
        const auto width = static_cast<std::int32_t>(call.argument(2));
        const auto height = static_cast<std::int32_t>(call.argument(3));
        if (width < 0 || height < 0) {
            set_gl_error(call, gles_abi::invalid_value);
            return;
        }
        context->scissor_box = {static_cast<std::int32_t>(call.argument(0)),
                                static_cast<std::int32_t>(call.argument(1)),
                                width, height};
    });
    add("_glColorMask", [this](UserlandHleCall& call) {
        auto* context = current_context(call);
        if (context == nullptr) {
            set_gl_error(call, gles_abi::invalid_operation);
            return;
        }
        for (std::size_t component = 0; component < context->color_mask.size();
             ++component) {
            context->color_mask[component] = call.argument(component) != 0;
        }
    });
    add("_glDepthMask", [this](UserlandHleCall& call) {
        auto* context = current_context(call);
        if (context == nullptr) {
            set_gl_error(call, gles_abi::invalid_operation);
        } else {
            context->depth_mask = call.argument(0) != 0;
        }
    });
    add("_glStencilMask", [this](UserlandHleCall& call) {
        auto* context = current_context(call);
        if (context == nullptr) {
            set_gl_error(call, gles_abi::invalid_operation);
        } else {
            context->stencil_mask = call.argument(0);
        }
    });
    add("_glFrontFace", [this](UserlandHleCall& call) {
        auto* context = current_context(call);
        if (context == nullptr) {
            set_gl_error(call, gles_abi::invalid_operation);
        } else if (call.argument(0) != gles_abi::clockwise &&
                   call.argument(0) != gles_abi::counter_clockwise) {
            set_gl_error(call, gles_abi::invalid_enum);
        } else {
            context->front_face = call.argument(0);
        }
    });
    add("_glCullFace", [this](UserlandHleCall& call) {
        auto* context = current_context(call);
        if (context == nullptr) {
            set_gl_error(call, gles_abi::invalid_operation);
        } else if (call.argument(0) != gles_abi::front &&
                   call.argument(0) != gles_abi::back &&
                   call.argument(0) != gles_abi::front_and_back) {
            set_gl_error(call, gles_abi::invalid_enum);
        } else {
            context->cull_mode = call.argument(0);
        }
    });
    add("_glEnable", [this](UserlandHleCall& call) {
        auto* context = current_context(call);
        if (context == nullptr) {
            set_gl_error(call, gles_abi::invalid_operation);
            return;
        }
        auto& unit = context->texture_units[context->active_texture_unit];
        if (call.argument(0) == gles_abi::texture_2d) {
            unit.texture_2d_enabled = true;
        } else if (call.argument(0) == gles_abi::texture_rectangle_apple) {
            unit.texture_rectangle_enabled = true;
        } else {
            context->enabled_capabilities.insert(call.argument(0));
        }
    });
    add("_glDisable", [this](UserlandHleCall& call) {
        auto* context = current_context(call);
        if (context == nullptr) {
            set_gl_error(call, gles_abi::invalid_operation);
            return;
        }
        auto& unit = context->texture_units[context->active_texture_unit];
        if (call.argument(0) == gles_abi::texture_2d) {
            unit.texture_2d_enabled = false;
        } else if (call.argument(0) == gles_abi::texture_rectangle_apple) {
            unit.texture_rectangle_enabled = false;
        } else {
            context->enabled_capabilities.erase(call.argument(0));
        }
    });
    add("_glIsEnabled", [this](UserlandHleCall& call) {
        const auto* context = current_context(call);
        if (context == nullptr) {
            call.set_return(0);
            return;
        }
        const auto& unit = context->texture_units[context->active_texture_unit];
        const auto enabled =
            call.argument(0) == gles_abi::texture_2d ? unit.texture_2d_enabled
            : call.argument(0) == gles_abi::texture_rectangle_apple
                ? unit.texture_rectangle_enabled
                : context->enabled_capabilities.contains(call.argument(0));
        call.set_return(enabled ? 1U : 0U);
    });
    const auto generate_names = [this](UserlandHleCall& call) {
        const auto signed_count = static_cast<std::int32_t>(call.argument(0));
        const auto output = call.argument(1);
        if (current_context(call) == nullptr) {
            set_gl_error(call, gles_abi::invalid_operation);
            return;
        }
        if (signed_count < 0 || (signed_count != 0 && output == 0)) {
            set_gl_error(call, gles_abi::invalid_value);
            return;
        }
        const auto texture_names = call.symbol() == "_glGenTextures";
        const auto count = static_cast<std::uint32_t>(signed_count);
        for (std::uint32_t index = 0; index < count; ++index) {
            const auto name = texture_names ? resources_.generate_texture()
                                            : resources_.generate_buffer();
            if (!call.memory().write32(output + index * 4U, name)) {
                set_gl_error(call, gl_invalid_value);
                break;
            }
        }
    };
    add("_glGenTextures", generate_names);
    add("_glGenBuffers", generate_names);
    add("_glIsTexture", [this](UserlandHleCall& call) {
        call.set_return(resources_.has_texture(call.argument(0)) ? 1U : 0U);
    });
    add("_glIsBuffer", [this](UserlandHleCall& call) {
        call.set_return(resources_.has_buffer(call.argument(0)) ? 1U : 0U);
    });
    add("_glBindTexture", [this](UserlandHleCall& call) {
        auto* context = current_context(call);
        if (context == nullptr) {
            set_gl_error(call, gles_abi::invalid_operation);
            return;
        }
        const auto target = call.argument(0);
        if (target != gles_abi::texture_2d &&
            target != gles_abi::texture_rectangle_apple) {
            set_gl_error(call, gles_abi::invalid_enum);
            return;
        }
        resources_.ensure_texture(call.argument(1));
        auto& unit = context->texture_units[context->active_texture_unit];
        auto& binding = target == gles_abi::texture_rectangle_apple
                            ? unit.bound_texture_rectangle
                            : unit.bound_texture_2d;
        binding = call.argument(1);
    });
    add("_glBindBuffer", [this](UserlandHleCall& call) {
        auto* context = current_context(call);
        if (context == nullptr) {
            set_gl_error(call, gles_abi::invalid_operation);
            return;
        }
        const auto target = call.argument(0);
        if (target != gles_abi::array_buffer &&
            target != gles_abi::element_array_buffer) {
            set_gl_error(call, gles_abi::invalid_enum);
            return;
        }
        resources_.ensure_buffer(call.argument(1));
        auto& binding = target == gles_abi::array_buffer
                            ? context->bound_array_buffer
                            : context->bound_element_array_buffer;
        binding = call.argument(1);
    });
    const auto delete_names = [this](UserlandHleCall& call) {
        const auto signed_count = static_cast<std::int32_t>(call.argument(0));
        const auto input = call.argument(1);
        if (current_context(call) == nullptr) {
            set_gl_error(call, gles_abi::invalid_operation);
            return;
        }
        if (signed_count < 0 || (signed_count != 0 && input == 0)) {
            set_gl_error(call, gles_abi::invalid_value);
            return;
        }
        const auto texture_names = call.symbol() == "_glDeleteTextures";
        const auto count = static_cast<std::uint32_t>(signed_count);
        for (std::uint32_t index = 0; index < count; ++index) {
            const auto name = call.memory().read32(input + index * 4U);
            if (!name) {
                set_gl_error(call, gl_invalid_value);
                break;
            }
            if (texture_names) {
                resources_.erase_texture(*name);
                for (auto& [context_name, context] : contexts_) {
                    static_cast<void>(context_name);
                    for (auto& unit : context.texture_units) {
                        if (unit.bound_texture_2d == *name) {
                            unit.bound_texture_2d = 0;
                        }
                        if (unit.bound_texture_rectangle == *name) {
                            unit.bound_texture_rectangle = 0;
                        }
                    }
                }
            } else {
                resources_.erase_buffer(*name);
                for (auto& [context_name, context] : contexts_) {
                    static_cast<void>(context_name);
                    if (context.bound_array_buffer == *name) {
                        context.bound_array_buffer = 0;
                    }
                    if (context.bound_element_array_buffer == *name) {
                        context.bound_element_array_buffer = 0;
                    }
                }
            }
        }
    };
    add("_glDeleteTextures", delete_names);
    add("_glDeleteBuffers", delete_names);
    add("_glPixelStorei", [this](UserlandHleCall& call) {
        auto* context = current_context(call);
        if (context == nullptr) {
            set_gl_error(call, gles_abi::invalid_operation);
            return;
        }
        const auto alignment = call.argument(1);
        if (alignment != 1 && alignment != 2 && alignment != 4 &&
            alignment != 8) {
            set_gl_error(call, gles_abi::invalid_value);
            return;
        }
        if (call.argument(0) == gles_abi::unpack_alignment) {
            context->unpack_alignment = alignment;
        } else if (call.argument(0) == gles_abi::pack_alignment) {
            context->pack_alignment = alignment;
        } else {
            set_gl_error(call, gles_abi::invalid_enum);
        }
    });
    add("_glTexImage2D", [this](UserlandHleCall& call) {
        auto* context = current_context(call);
        const auto level = static_cast<std::int32_t>(call.argument(1));
        const auto width = static_cast<std::int32_t>(call.argument(3));
        const auto height = static_cast<std::int32_t>(call.argument(4));
        const auto border = static_cast<std::int32_t>(call.argument(5));
        if (context == nullptr) {
            set_gl_error(call, gles_abi::invalid_operation);
            return;
        }
        const auto target = call.argument(0);
        if (target != gles_abi::texture_2d &&
            target != gles_abi::texture_rectangle_apple) {
            set_gl_error(call, gles_abi::invalid_enum);
            return;
        }
        if (level < 0 || width < 0 || height < 0 || border != 0) {
            set_gl_error(call, gles_abi::invalid_value);
            return;
        }
        const auto& unit = context->texture_units[context->active_texture_unit];
        const auto binding = target == gles_abi::texture_rectangle_apple
                                 ? unit.bound_texture_rectangle
                                 : unit.bound_texture_2d;
        const auto error = resources_.upload_texture_2d(
            call.memory(), binding, static_cast<std::uint32_t>(level),
            call.argument(2), static_cast<std::uint32_t>(width),
            static_cast<std::uint32_t>(height), call.argument(6),
            call.argument(7), call.argument(8), context->unpack_alignment);
        if (error != gles_abi::no_error)
            set_gl_error(call, error);
    });
    add("_glTexSubImage2D", [this](UserlandHleCall& call) {
        auto* context = current_context(call);
        const auto level = static_cast<std::int32_t>(call.argument(1));
        const auto x = static_cast<std::int32_t>(call.argument(2));
        const auto y = static_cast<std::int32_t>(call.argument(3));
        const auto width = static_cast<std::int32_t>(call.argument(4));
        const auto height = static_cast<std::int32_t>(call.argument(5));
        if (context == nullptr) {
            set_gl_error(call, gles_abi::invalid_operation);
            return;
        }
        const auto target = call.argument(0);
        if (target != gles_abi::texture_2d &&
            target != gles_abi::texture_rectangle_apple) {
            set_gl_error(call, gles_abi::invalid_enum);
            return;
        }
        if (level < 0 || x < 0 || y < 0 || width < 0 || height < 0) {
            set_gl_error(call, gles_abi::invalid_value);
            return;
        }
        const auto& unit = context->texture_units[context->active_texture_unit];
        const auto binding = target == gles_abi::texture_rectangle_apple
                                 ? unit.bound_texture_rectangle
                                 : unit.bound_texture_2d;
        const auto error = resources_.update_texture_2d(
            call.memory(), binding, static_cast<std::uint32_t>(level),
            static_cast<std::uint32_t>(x), static_cast<std::uint32_t>(y),
            static_cast<std::uint32_t>(width),
            static_cast<std::uint32_t>(height), call.argument(6),
            call.argument(7), call.argument(8), context->unpack_alignment);
        if (error != gles_abi::no_error)
            set_gl_error(call, error);
    });
    const auto set_texture_parameter = [this](UserlandHleCall& call) {
        auto* context = current_context(call);
        if (context == nullptr) {
            set_gl_error(call, gles_abi::invalid_operation);
            return;
        }
        const auto target = call.argument(0);
        if (target != gles_abi::texture_2d &&
            target != gles_abi::texture_rectangle_apple) {
            set_gl_error(call, gles_abi::invalid_enum);
            return;
        }
        const auto& unit = context->texture_units[context->active_texture_unit];
        const auto binding = target == gles_abi::texture_rectangle_apple
                                 ? unit.bound_texture_rectangle
                                 : unit.bound_texture_2d;
        const auto error = resources_.set_texture_parameter(
            binding, call.argument(1), call.argument(2));
        if (error != gles_abi::no_error)
            set_gl_error(call, error);
    };
    add("_glTexParameteri", set_texture_parameter);
    add("_glTexParameterf", set_texture_parameter);
    add("_glTexParameterx", set_texture_parameter);
    const auto set_texture_environment = [this](UserlandHleCall& call,
                                                float parameter) {
        auto* context = current_context(call);
        if (context == nullptr) {
            set_gl_error(call, gles_abi::invalid_operation);
            return false;
        }
        if (call.argument(0) != gles_abi::texture_environment ||
            !std::isfinite(parameter) || parameter < 0.0F ||
            parameter >
                static_cast<float>(std::numeric_limits<std::uint32_t>::max())) {
            set_gl_error(call, gles_abi::invalid_enum);
            return false;
        }
        auto& environment = context->texture_units[context->active_texture_unit]
                                .texture_environment;
        const auto name = call.argument(1);
        const auto value = static_cast<std::uint32_t>(std::lround(parameter));
        const auto is_one_of = [](std::uint32_t candidate, auto... values) {
            return ((candidate == values) || ...);
        };
        if (name == gles_abi::texture_environment_mode) {
            if (!is_one_of(value, gles_abi::replace, gles_abi::modulate,
                           gles_abi::decal, gles_abi::blend, gles_abi::add,
                           gles_abi::combine)) {
                set_gl_error(call, gles_abi::invalid_enum);
                return false;
            }
            environment.mode = value;
            return true;
        }
        if (name == gles_abi::combine_rgb) {
            if (!is_one_of(value, gles_abi::replace, gles_abi::modulate,
                           gles_abi::add, gles_abi::add_signed,
                           gles_abi::interpolate, gles_abi::subtract,
                           gles_abi::dot3_rgb, gles_abi::dot3_rgba)) {
                set_gl_error(call, gles_abi::invalid_enum);
                return false;
            }
            environment.combine_rgb = value;
            return true;
        }
        if (name == gles_abi::combine_alpha) {
            if (!is_one_of(value, gles_abi::replace, gles_abi::modulate,
                           gles_abi::add, gles_abi::add_signed,
                           gles_abi::interpolate, gles_abi::subtract)) {
                set_gl_error(call, gles_abi::invalid_enum);
                return false;
            }
            environment.combine_alpha = value;
            return true;
        }
        const auto source_index =
            name >= gles_abi::source0_rgb && name <= gles_abi::source2_rgb
                ? std::optional<std::size_t>{name - gles_abi::source0_rgb}
                : std::nullopt;
        const auto alpha_source_index =
            name >= gles_abi::source0_alpha && name <= gles_abi::source2_alpha
                ? std::optional<std::size_t>{name - gles_abi::source0_alpha}
                : std::nullopt;
        if (source_index || alpha_source_index) {
            if (!is_one_of(value, gles_abi::texture_source, gles_abi::constant,
                           gles_abi::primary_color, gles_abi::previous)) {
                set_gl_error(call, gles_abi::invalid_enum);
                return false;
            }
            if (source_index) {
                environment.rgb_sources[*source_index] = value;
            } else {
                environment.alpha_sources[*alpha_source_index] = value;
            }
            return true;
        }
        const auto operand_index =
            name >= gles_abi::operand0_rgb && name <= gles_abi::operand2_rgb
                ? std::optional<std::size_t>{name - gles_abi::operand0_rgb}
                : std::nullopt;
        const auto alpha_operand_index =
            name >= gles_abi::operand0_alpha && name <= gles_abi::operand2_alpha
                ? std::optional<std::size_t>{name - gles_abi::operand0_alpha}
                : std::nullopt;
        if (operand_index) {
            if (!is_one_of(value, gles_abi::source_color,
                           gles_abi::one_minus_source_color,
                           gles_abi::source_alpha,
                           gles_abi::one_minus_source_alpha)) {
                set_gl_error(call, gles_abi::invalid_enum);
                return false;
            }
            environment.rgb_operands[*operand_index] = value;
            return true;
        }
        if (alpha_operand_index) {
            if (!is_one_of(value, gles_abi::source_alpha,
                           gles_abi::one_minus_source_alpha)) {
                set_gl_error(call, gles_abi::invalid_enum);
                return false;
            }
            environment.alpha_operands[*alpha_operand_index] = value;
            return true;
        }
        if (name == gles_abi::rgb_scale || name == gles_abi::alpha_scale) {
            if (parameter != 1.0F && parameter != 2.0F && parameter != 4.0F) {
                set_gl_error(call, gles_abi::invalid_value);
                return false;
            }
            if (name == gles_abi::rgb_scale) {
                environment.rgb_scale = parameter;
            } else {
                environment.alpha_scale = parameter;
            }
            return true;
        }
        set_gl_error(call, gles_abi::invalid_enum);
        return false;
    };
    add("_glTexEnvi", [set_texture_environment](UserlandHleCall& call) {
        set_texture_environment(call, static_cast<float>(call.argument(2)));
    });
    add("_glTexEnvfv", [this, set_texture_environment](UserlandHleCall& call) {
        auto* context = current_context(call);
        const auto values = call.argument(2);
        if (context == nullptr || values == 0) {
            set_gl_error(call, context == nullptr ? gles_abi::invalid_operation
                                                  : gles_abi::invalid_value);
            return;
        }
        if (call.argument(0) != gles_abi::texture_environment) {
            set_gl_error(call, gles_abi::invalid_enum);
            return;
        }
        auto& environment = context->texture_units[context->active_texture_unit]
                                .texture_environment;
        if (call.argument(1) == gles_abi::texture_environment_color) {
            for (std::size_t component = 0;
                 component < environment.color.size(); ++component) {
                const auto raw = call.memory().read32(
                    values + static_cast<std::uint32_t>(component * 4U));
                if (!raw) {
                    set_gl_error(call, gles_abi::invalid_value);
                    return;
                }
                const auto value = std::bit_cast<float>(*raw);
                if (!std::isfinite(value)) {
                    set_gl_error(call, gles_abi::invalid_value);
                    return;
                }
                environment.color[component] = std::clamp(value, 0.0F, 1.0F);
            }
            return;
        }
        const auto raw = call.memory().read32(values);
        if (!raw) {
            set_gl_error(call, gles_abi::invalid_value);
            return;
        }
        set_texture_environment(call, std::bit_cast<float>(*raw));
    });
    add("_glTexImageCoreSurfaceAPPLE", [this](UserlandHleCall& call) {
        auto* context = current_context(call);
        if (context == nullptr) {
            set_gl_error(call, gles_abi::invalid_operation);
            return;
        }
        if (call.argument(0) != gles_abi::texture_rectangle_apple) {
            set_gl_error(call, gles_abi::invalid_enum);
            return;
        }
        const auto identifier = core_surface_identifier(call, call.argument(1));
        if (!identifier) {
            set_gl_error(call, gles_abi::invalid_value);
            return;
        }
        const auto error = resources_.import_surface_texture(
            call.memory(),
            context->texture_units[context->active_texture_unit]
                .bound_texture_rectangle,
            *surface_store_, *identifier);
        if (error != gles_abi::no_error)
            set_gl_error(call, error);
    });
    add("_glFinishTextureAPPLE", [this](UserlandHleCall& call) {
        auto* context = current_context(call);
        if (context == nullptr) {
            set_gl_error(call, gles_abi::invalid_operation);
            return;
        }
        const auto target = call.argument(0);
        if (target != gles_abi::texture_2d &&
            target != gles_abi::texture_rectangle_apple) {
            set_gl_error(call, gles_abi::invalid_enum);
            return;
        }
        const auto& unit = context->texture_units[context->active_texture_unit];
        const auto binding = target == gles_abi::texture_rectangle_apple
                                 ? unit.bound_texture_rectangle
                                 : unit.bound_texture_2d;
        const auto error = resources_.refresh_surface_texture(
            call.memory(), binding, *surface_store_);
        if (error != gles_abi::no_error)
            set_gl_error(call, error);
    });
    add("_glBufferData", [this](UserlandHleCall& call) {
        auto* context = current_context(call);
        const auto size = static_cast<std::int32_t>(call.argument(1));
        if (context == nullptr) {
            set_gl_error(call, gles_abi::invalid_operation);
            return;
        }
        const auto target = call.argument(0);
        if (target != gles_abi::array_buffer &&
            target != gles_abi::element_array_buffer) {
            set_gl_error(call, gles_abi::invalid_enum);
            return;
        }
        if (size < 0) {
            set_gl_error(call, gles_abi::invalid_value);
            return;
        }
        if (call.argument(3) != gles_abi::static_draw &&
            call.argument(3) != gles_abi::dynamic_draw) {
            set_gl_error(call, gles_abi::invalid_enum);
            return;
        }
        const auto binding = target == gles_abi::array_buffer
                                 ? context->bound_array_buffer
                                 : context->bound_element_array_buffer;
        const auto error = resources_.upload_buffer(
            call.memory(), binding, static_cast<std::uint32_t>(size),
            call.argument(2), call.argument(3));
        if (error != gles_abi::no_error)
            set_gl_error(call, error);
    });
    add("_glBufferSubData", [this](UserlandHleCall& call) {
        auto* context = current_context(call);
        const auto offset = static_cast<std::int32_t>(call.argument(1));
        const auto size = static_cast<std::int32_t>(call.argument(2));
        if (context == nullptr) {
            set_gl_error(call, gles_abi::invalid_operation);
            return;
        }
        const auto target = call.argument(0);
        if (target != gles_abi::array_buffer &&
            target != gles_abi::element_array_buffer) {
            set_gl_error(call, gles_abi::invalid_enum);
            return;
        }
        if (offset < 0 || size < 0) {
            set_gl_error(call, gles_abi::invalid_value);
            return;
        }
        const auto binding = target == gles_abi::array_buffer
                                 ? context->bound_array_buffer
                                 : context->bound_element_array_buffer;
        const auto error = resources_.update_buffer(
            call.memory(), binding, static_cast<std::uint32_t>(offset),
            static_cast<std::uint32_t>(size), call.argument(3));
        if (error != gles_abi::no_error)
            set_gl_error(call, error);
    });
    add("_glGetBufferParameteriv", [this](UserlandHleCall& call) {
        auto* context = current_context(call);
        if (context == nullptr) {
            set_gl_error(call, gles_abi::invalid_operation);
            return;
        }
        const auto target = call.argument(0);
        if (target != gles_abi::array_buffer &&
            target != gles_abi::element_array_buffer) {
            set_gl_error(call, gles_abi::invalid_enum);
            return;
        }
        const auto binding = target == gles_abi::array_buffer
                                 ? context->bound_array_buffer
                                 : context->bound_element_array_buffer;
        const auto* buffer = resources_.buffer(binding);
        if (buffer == nullptr) {
            set_gl_error(call, gles_abi::invalid_operation);
            return;
        }
        if (call.argument(2) == 0) {
            set_gl_error(call, gles_abi::invalid_value);
            return;
        }
        std::uint32_t value{};
        if (call.argument(1) == gles_abi::buffer_size) {
            value = static_cast<std::uint32_t>(buffer->bytes.size());
        } else if (call.argument(1) == gles_abi::buffer_usage) {
            value = buffer->usage;
        } else {
            set_gl_error(call, gles_abi::invalid_enum);
            return;
        }
        if (!call.write32(call.argument(2), value)) {
            set_gl_error(call, gles_abi::invalid_value);
        }
    });
    add("_glVertexPointer", [this](UserlandHleCall& call) {
        set_array_pointer(call, gles_abi::vertex_array);
    });
    add("_glColorPointer", [this](UserlandHleCall& call) {
        set_array_pointer(call, gles_abi::color_array);
    });
    add("_glTexCoordPointer", [this](UserlandHleCall& call) {
        set_array_pointer(call, gles_abi::texture_coord_array);
    });
    const auto set_client_array = [this](UserlandHleCall& call) {
        auto* context = current_context(call);
        if (context == nullptr) {
            set_gl_error(call, gles_abi::invalid_operation);
            return;
        }
        ContextState::ArrayPointer* array{};
        if (call.argument(0) == gles_abi::vertex_array) {
            array = &context->vertex_array;
        } else if (call.argument(0) == gles_abi::color_array) {
            array = &context->color_array;
        } else if (call.argument(0) == gles_abi::texture_coord_array) {
            array = &context->texture_units[context->client_active_texture_unit]
                         .texture_array;
        } else {
            set_gl_error(call, gles_abi::invalid_enum);
            return;
        }
        array->enabled = call.symbol() == "_glEnableClientState";
    };
    add("_glEnableClientState", set_client_array);
    add("_glDisableClientState", set_client_array);
    add("_glBlendFunc", [this](UserlandHleCall& call) {
        auto* context = current_context(call);
        if (context == nullptr) {
            set_gl_error(call, gles_abi::invalid_operation);
            return;
        }
        context->blend_source = call.argument(0);
        context->blend_destination = call.argument(1);
    });
    add("_glDrawArrays", [this](UserlandHleCall& call) { draw(call, false); });
    add("_glDrawElements", [this](UserlandHleCall& call) { draw(call, true); });
    add("_glFlush", [this](UserlandHleCall& call) {
        const auto surface = thread(call).draw_surface;
        if (surface != 0 &&
            !renderer_->flush(render_target_key(surface))) {
            set_gl_error(call, gles_abi::invalid_operation);
        }
    });
    add("_glFinish", [this](UserlandHleCall& call) {
        const auto surface = thread(call).draw_surface;
        if (surface != 0 &&
            !renderer_->finish(render_target_key(surface))) {
            set_gl_error(call, gles_abi::invalid_operation);
        }
    });
    registry.register_prefix(
        std::string{opengles_image}, "_gl",
        [this](UserlandHleCall& call) { unsupported(call); });
}

void OpenGlesHle::unsupported(UserlandHleCall& call) {
    // Most GLES 1.1 state setters are safe to defer until the renderer consumes
    // them. The public prefix hook is still essential: it prevents any call
    // from falling through into the PowerVR command-stream implementation.
    if (unsupported_trace_count_ < maximum_unsupported_traces) {
        call.output().write(
            "[opengles] deferred symbol=" + std::string{call.symbol()} +
            " pid=" + std::to_string(call.process_id()) + "\n");
        ++unsupported_trace_count_;
    }
    call.set_return(0);
}

} // namespace ilemu
