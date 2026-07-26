#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <vector>

#include "ilemu/display.hpp"
#include "ilemu/gles_abi.hpp"
#include "ilemu/gles_math.hpp"
#include "ilemu/gles_rasterizer.hpp"
#include "ilemu/gles_renderer.hpp"
#include "ilemu/gles_resources.hpp"

namespace ilemu {

class UserlandHleCall;
class UserlandHleRegistry;
class SceneCoordinator;
class SurfaceStore;
struct KernelSharedState;

// High-level implementation of the public iPhoneOS 1.0 OpenGLES framework
// ABI. It deliberately models EGL/GLES state rather than the PowerVR MBX
// kernel command stream used by Apple's original implementation.
class OpenGlesHle {
  public:
    OpenGlesHle(UserlandHleRegistry& registry,
                std::shared_ptr<DisplayState> display,
                std::shared_ptr<SurfaceStore> surfaces = {});
    ~OpenGlesHle();

    void reset();
    void inherit_state(const OpenGlesHle& parent);
    void set_display(std::shared_ptr<DisplayState> display);
    void set_shared_state(std::shared_ptr<KernelSharedState> shared_state);
    void set_scene_coordinator(std::shared_ptr<SceneCoordinator> scenes);
    [[nodiscard]] const GlesResourceStore& resources() const {
        return resources_;
    }

  private:
    struct ThreadState {
        std::uint32_t display{};
        std::uint32_t draw_surface{};
        std::uint32_t read_surface{};
        std::uint32_t context{};
        std::uint32_t gl_error{};
    };
    struct SurfaceState {
        std::optional<std::uint32_t> backing_identifier;
        std::uint32_t width{};
        std::uint32_t height{};
        std::vector<std::uint32_t> pixels;
        std::set<std::uint32_t> refreshed_textures;
        bool dirty{};
    };
    struct ContextState {
        struct ArrayPointer {
            std::uint32_t size{};
            std::uint32_t type{};
            std::uint32_t stride{};
            std::uint32_t pointer{};
            std::uint32_t buffer{};
            bool enabled{};
        };
        struct TextureUnitState {
            std::uint32_t bound_texture_2d{};
            std::uint32_t bound_texture_rectangle{};
            GlesTextureEnvironment texture_environment;
            GlesMatrix texture_matrix;
            std::vector<GlesMatrix> texture_stack;
            ArrayPointer texture_array;
            bool texture_2d_enabled{};
            bool texture_rectangle_enabled{};
        };
        std::array<TextureUnitState, gles_abi::texture_unit_count>
            texture_units;
        std::uint32_t bound_array_buffer{};
        std::uint32_t bound_element_array_buffer{};
        std::uint32_t unpack_alignment{gles_abi::default_pixel_alignment};
        std::uint32_t pack_alignment{gles_abi::default_pixel_alignment};
        std::array<std::int32_t, 4> viewport{};
        std::array<std::int32_t, 4> scissor_box{};
        std::array<float, 4> current_color{1.0F, 1.0F, 1.0F, 1.0F};
        std::array<bool, 4> color_mask{true, true, true, true};
        std::array<std::uint32_t, 4> clear_color{};
        std::uint32_t clear_argb{0xff000000U};
        std::set<std::uint32_t> enabled_capabilities;
        std::uint32_t blend_source{gles_abi::one};
        std::uint32_t blend_destination{gles_abi::zero};
        std::size_t active_texture_unit{};
        std::size_t client_active_texture_unit{};
        std::uint32_t cull_mode{gles_abi::back};
        std::uint32_t front_face{gles_abi::counter_clockwise};
        std::uint32_t stencil_mask{0xffffffffU};
        bool depth_mask{true};
        std::uint32_t matrix_mode{gles_abi::modelview};
        GlesMatrix modelview_matrix;
        GlesMatrix projection_matrix;
        std::vector<GlesMatrix> modelview_stack;
        std::vector<GlesMatrix> projection_stack;
        ArrayPointer vertex_array;
        ArrayPointer color_array;
    };

    [[nodiscard]] ThreadState& thread(UserlandHleCall& call);
    [[nodiscard]] ContextState* current_context(UserlandHleCall& call);
    void set_gl_error(UserlandHleCall& call, std::uint32_t error);
    void set_array_pointer(UserlandHleCall& call, std::uint32_t array);
    [[nodiscard]] GlesMatrix* current_matrix(ContextState& context);
    [[nodiscard]] std::vector<GlesMatrix>*
    current_matrix_stack(ContextState& context);
    void multiply_current_matrix(UserlandHleCall& call, GlesMatrix matrix);
    [[nodiscard]] bool read_array(UserlandHleCall& call,
                                  const ContextState::ArrayPointer& array,
                                  std::uint32_t index,
                                  std::span<float> destination,
                                  bool normalized) const;
    [[nodiscard]] std::optional<GlesRasterVertex>
    read_vertex(UserlandHleCall& call, const ContextState& context,
                std::uint32_t index) const;
    [[nodiscard]] std::optional<std::uint32_t>
    core_surface_identifier(UserlandHleCall& call, std::uint32_t surface) const;
    [[nodiscard]] SurfaceState* current_pixmap_surface(UserlandHleCall& call);
    [[nodiscard]] GlesRenderTargetKey
    render_target_key(std::uint32_t surface) const;
    void release_unbacked_surfaces();
    [[nodiscard]] bool reload_surface(UserlandHleCall& call,
                                      std::uint32_t surface);
    [[nodiscard]] bool flush_surface(UserlandHleCall& call,
                                     std::uint32_t surface);
    [[nodiscard]] std::optional<DisplayFrame>
    render_target(UserlandHleCall& call);
    [[nodiscard]] bool commit_render_target(UserlandHleCall& call,
                                            DisplayFrame frame);
    void draw(UserlandHleCall& call, bool indexed);
    [[nodiscard]] bool display_write_allowed(UserlandHleCall& call) const;
    void register_egl(UserlandHleRegistry& registry);
    void register_gles(UserlandHleRegistry& registry);
    void unsupported(UserlandHleCall& call);

    std::map<std::size_t, ThreadState> threads_;
    std::map<std::uint32_t, ContextState> contexts_;
    std::map<std::uint32_t, SurfaceState> surfaces_;
    GlesResourceStore resources_;
    std::uint32_t next_context_{0x00010001U};
    std::uint32_t next_surface_{0x00020001U};
    std::uint32_t egl_error_{0x3000U};
    std::uint64_t frame_count_{};
    std::size_t unsupported_trace_count_{};
    std::shared_ptr<DisplayState> display_;
    std::shared_ptr<SurfaceStore> surface_store_;
    std::uint64_t renderer_owner_{};
    std::shared_ptr<GlesRenderer> renderer_;
    std::unique_ptr<CommandEncoder> command_encoder_;
    std::shared_ptr<KernelSharedState> shared_state_;
    std::shared_ptr<SceneCoordinator> scene_coordinator_;
};

} // namespace ilemu
