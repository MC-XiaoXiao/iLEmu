#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <string_view>
#include <vector>

#include "ilemu/display.hpp"
#include "ilemu/gles_abi.hpp"
#include "ilemu/gles_math.hpp"
#include "ilemu/gles_program_state.hpp"
#include "ilemu/gles_rasterizer.hpp"
#include "ilemu/gles_renderer.hpp"
#include "ilemu/gles_resources.hpp"
#include "ilemu/opengles_guest_profile.hpp"
#include "ilemu/scanout_composition.hpp"

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
        std::shared_ptr<SurfaceStore> surfaces = { });
    ~OpenGlesHle();

    void reset();
    void inherit_state(const OpenGlesHle& parent);
    void set_display(std::shared_ptr<DisplayState> display);
    void set_guest_profile(OpenGlesGuestProfileKind profile);
    void set_shared_state(std::shared_ptr<KernelSharedState> shared_state);
    void set_scene_coordinator(std::shared_ptr<SceneCoordinator> scenes);
    [[nodiscard]] const GlesResourceStore& resources() const
    {
        return resources_;
    }

private:
    struct ThreadState {
        std::uint32_t display { };
        std::uint32_t draw_surface { };
        std::uint32_t read_surface { };
        std::uint32_t context { };
        std::uint32_t gl_error { };
    };
    struct SurfaceState {
        std::optional<std::uint32_t> backing_identifier;
        std::uint32_t width { };
        std::uint32_t height { };
        std::vector<std::uint32_t> pixels;
        std::uint64_t backing_cpu_generation { };
        std::set<std::uint32_t> refreshed_textures;
        bool dirty { };
    };
    struct ContextState {
        struct FramebufferState {
            std::uint32_t color_texture_target { };
            std::uint32_t color_texture { };
            std::uint32_t color_renderbuffer { };
        };
        struct RenderbufferState {
            std::uint32_t width { };
            std::uint32_t height { };
            std::uint32_t internal_format { };
            std::uint32_t color_texture { };
        };
        struct ArrayPointer {
            std::uint32_t size { };
            std::uint32_t type { };
            std::uint32_t stride { };
            std::uint32_t pointer { };
            std::uint32_t buffer { };
            bool normalized { };
            bool enabled { };
        };
        struct TextureUnitState {
            std::uint32_t bound_texture_2d { };
            std::uint32_t bound_texture_rectangle { };
            GlesTextureEnvironment texture_environment;
            GlesMatrix texture_matrix;
            std::vector<GlesMatrix> texture_stack;
            ArrayPointer texture_array;
            bool texture_2d_enabled { };
            bool texture_rectangle_enabled { };
        };
        std::array<TextureUnitState, gles_abi::texture_unit_count>
            texture_units;
        OpenGlesGuestProfileKind guest_profile_kind {
            OpenGlesGuestProfileKind::MbxLiteLegacy
        };
        std::uint32_t bound_array_buffer { };
        std::uint32_t bound_element_array_buffer { };
        std::uint32_t bound_framebuffer { };
        std::uint32_t bound_renderbuffer { };
        std::map<std::uint32_t, FramebufferState> framebuffers;
        std::map<std::uint32_t, RenderbufferState> renderbuffers;
        std::uint32_t unpack_alignment { gles_abi::default_pixel_alignment };
        std::uint32_t pack_alignment { gles_abi::default_pixel_alignment };
        std::array<std::int32_t, 4> viewport { };
        std::array<std::int32_t, 4> scissor_box { };
        std::array<float, 4> current_color { 1.0F, 1.0F, 1.0F, 1.0F };
        float line_width { 1.0F };
        std::array<bool, 4> color_mask { true, true, true, true };
        std::array<std::uint32_t, 4> clear_color { };
        std::uint32_t clear_argb { 0xff000000U };
        std::set<std::uint32_t> enabled_capabilities;
        std::uint32_t blend_source { gles_abi::one };
        std::uint32_t blend_destination { gles_abi::zero };
        std::size_t active_texture_unit { };
        std::size_t client_active_texture_unit { };
        std::uint32_t cull_mode { gles_abi::back };
        std::uint32_t front_face { gles_abi::counter_clockwise };
        std::uint32_t stencil_mask { 0xffffffffU };
        bool depth_mask { true };
        std::uint32_t matrix_mode { gles_abi::modelview };
        GlesMatrix modelview_matrix;
        GlesMatrix projection_matrix;
        std::vector<GlesMatrix> modelview_stack;
        std::vector<GlesMatrix> projection_stack;
        ArrayPointer vertex_array;
        ArrayPointer color_array;
        std::map<std::uint32_t, ArrayPointer> generic_arrays;
        std::uint32_t current_program { };
    };
    struct ProgrammableDrawState {
        ContextState::ArrayPointer position_array;
        ContextState::ArrayPointer color_array;
        std::array<ContextState::ArrayPointer, gles_abi::texture_unit_count>
            texture_arrays;
        GlesMatrix vertex_matrix;
        // Legacy Apple shaders encode a per-unit affine texture transform as
        // (scale.x, scale.y, offset.x, offset.y) in texmatN. Some drivers use
        // the older scale-only texscaleN spelling.
        std::array<std::array<float, 4>, gles_abi::texture_unit_count>
            texture_transforms { { { 1.0F, 1.0F, 0.0F, 0.0F },
                { 1.0F, 1.0F, 0.0F, 0.0F } } };
        std::array<GlesTextureEnvironment, gles_abi::texture_unit_count>
            texture_environments;
        std::array<bool, gles_abi::texture_unit_count> sampled_textures { };
        std::array<bool, gles_abi::texture_unit_count> rectangle_textures { };
    };
    enum class RenderTargetKind : std::uint8_t {
        Display,
        Pixmap,
        Framebuffer,
    };
    struct RenderTargetBinding {
        RenderTargetKind kind { RenderTargetKind::Display };
        GlesRenderTargetKey key;
        std::optional<std::uint32_t> backing_identifier;
        SurfaceState* pixmap_surface { };
        std::uint32_t framebuffer_texture { };
        std::shared_ptr<HostSurface> host_surface;
        bool inverted_vertical { };
        bool premultiplied { };
        SurfaceState* display_surface { };
    };

    [[nodiscard]] ThreadState& thread(UserlandHleCall& call);
    [[nodiscard]] ContextState* current_context(UserlandHleCall& call);
    [[nodiscard]] ContextState default_context_state() const;
    void set_gl_error(UserlandHleCall& call, std::uint32_t error);
    void set_array_pointer(UserlandHleCall& call, std::uint32_t array);
    [[nodiscard]] GlesMatrix* current_matrix(ContextState& context);
    [[nodiscard]] std::vector<GlesMatrix>* current_matrix_stack(
        ContextState& context);
    void multiply_current_matrix(UserlandHleCall& call, GlesMatrix matrix);
    [[nodiscard]] bool read_array(UserlandHleCall& call,
        const ContextState::ArrayPointer& array, std::uint32_t index,
        std::span<float> destination, bool normalized) const;
    [[nodiscard]] std::optional<GlesRasterVertex> read_vertex(
        UserlandHleCall& call, const ContextState& context, std::uint32_t index,
        const ProgrammableDrawState* programmable = nullptr) const;
    [[nodiscard]] std::optional<ProgrammableDrawState> programmable_draw_state(
        const ContextState& context) const;
    [[nodiscard]] std::optional<std::uint32_t> core_surface_identifier(
        UserlandHleCall& call, std::uint32_t surface) const;
    [[nodiscard]] SurfaceState* current_pixmap_surface(UserlandHleCall& call);
    [[nodiscard]] bool refresh_pixmap_surface(
        UserlandHleCall& call, std::uint32_t surface);
    [[nodiscard]] const GlesResourceStore::TextureLevel*
    current_framebuffer_level(const ContextState& context) const;
    [[nodiscard]] std::uint32_t ensure_renderbuffer_storage(
        ContextState& context, std::uint32_t name, std::uint32_t width,
        std::uint32_t height, std::uint32_t internal_format);
    [[nodiscard]] std::optional<RenderTargetBinding> resolve_render_target(
        UserlandHleCall& call, ContextState& context);
    [[nodiscard]] GlesRenderTargetKey render_target_key(
        std::uint32_t surface) const;
    void release_renderer_resources();
    [[nodiscard]] bool reload_surface(
        UserlandHleCall& call, std::uint32_t surface);
    [[nodiscard]] bool flush_surface(
        UserlandHleCall& call, std::uint32_t surface);
    [[nodiscard]] std::optional<DisplayFrame> render_target(
        UserlandHleCall& call, const RenderTargetBinding& binding);
    [[nodiscard]] bool commit_render_target(UserlandHleCall& call,
        const RenderTargetBinding& binding, DisplayFrame frame);
    [[nodiscard]] bool publish_display_surface(
        UserlandHleCall& call, const std::shared_ptr<HostSurface>& surface);
    [[nodiscard]] std::shared_ptr<HostSurface> acquire_compatibility_surface(
        HostSurfaceDescriptor descriptor);
    void draw(UserlandHleCall& call, bool indexed);
    [[nodiscard]] bool display_write_allowed(UserlandHleCall& call) const;
    void register_eagl(UserlandHleRegistry& registry);
    void register_egl(UserlandHleRegistry& registry);
    void register_gles(UserlandHleRegistry& registry);
    void register_programmable_gles(UserlandHleRegistry& registry);
    void unsupported(UserlandHleCall& call);

    std::map<std::size_t, ThreadState> threads_;
    std::map<std::uint32_t, ContextState> contexts_;
    std::map<std::pair<std::uint32_t, std::uint32_t>, std::uint32_t>
        eagl_contexts_;
    std::optional<EaglContextProfileKind> eagl_context_profile_;
    std::map<std::uint32_t, SurfaceState> surfaces_;
    SurfaceState compatibility_display_surface_;
    std::uint32_t compatibility_display_process_id_ { };
    ScanoutComposition scanout_composition_;
    GlesResourceStore resources_;
    GlesProgramState programs_;
    std::uint32_t next_context_ { 0x00010001U };
    std::uint32_t next_surface_ { 0x00020001U };
    std::uint32_t next_framebuffer_ { 1U };
    std::uint32_t next_renderbuffer_ { 1U };
    std::uint32_t egl_error_ { 0x3000U };
    std::uint64_t frame_count_ { };
    std::size_t unsupported_trace_count_ { };
    std::shared_ptr<DisplayState> display_;
    std::shared_ptr<SurfaceStore> surface_store_;
    std::uint64_t renderer_owner_ { };
    std::vector<std::shared_ptr<HostSurface>> compatibility_surfaces_;
    std::size_t compatibility_surface_index_ { };
    std::uint64_t next_compatibility_surface_ { };
    std::shared_ptr<GlesRenderer> renderer_;
    std::unique_ptr<CommandEncoder> command_encoder_;
    OpenGlesGuestProfileKind default_guest_profile_kind_ {
        OpenGlesGuestProfileKind::MbxLiteLegacy
    };
    std::shared_ptr<KernelSharedState> shared_state_;
    std::shared_ptr<SceneCoordinator> scene_coordinator_;
};

} // namespace ilemu
