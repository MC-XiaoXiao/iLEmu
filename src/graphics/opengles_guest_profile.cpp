#include "ilemu/opengles_guest_profile.hpp"

#include "ilemu/userland_hle.hpp"

namespace ilemu {
namespace {

    constexpr std::string_view common_vendor { "Imagination Technologies" };
    constexpr std::string_view common_renderer {
        "PowerVR MBXLite with VGPLite"
    };
    constexpr std::string_view common_version { "OpenGL ES-CM 1.1" };
    constexpr std::string_view sgx535_renderer { "PowerVR SGX 535" };
    constexpr std::string_view sgx535_version {
        "OpenGL ES-CM 1.1 IMGSGX535-31.4"
    };

    constexpr OpenGlesGuestProfile legacy_mbx_lite {
        "mbx-lite-legacy",
        common_vendor,
        common_renderer,
        common_version,
        "GL_APPLE_client_storage GL_APPLE_texture_rectangle "
        "GL_IMG_read_format GL_IMG_texture_compression_pvrtc "
        "GL_IMG_texture_env_enhanced_fixed_function "
        "GL_IMG_texture_format_BGRA8888 GL_IMG_texture_stream "
        "GL_IMG_user_clip_planes GL_IMG_vertex_program "
        "GL_OES_byte_coordinates GL_OES_compressed_paletted_texture "
        "GL_OES_draw_texture GL_OES_fixed_point GL_OES_matrix_palette "
        "GL_OES_point_size_array GL_OES_point_sprite GL_OES_query_matrix "
        "GL_OES_read_format GL_OES_single_precision",
        2048,
        2048,
    };

    constexpr OpenGlesGuestProfile framebuffer_object_mbx_lite {
        "mbx-lite-framebuffer-object",
        common_vendor,
        common_renderer,
        common_version,
        "GL_EXT_texture_filter_anisotropic GL_EXT_texture_lod_bias "
        "GL_IMG_read_format GL_IMG_texture_compression_pvrtc "
        "GL_IMG_texture_format_BGRA8888 GL_OES_blend_subtract "
        "GL_OES_compressed_paletted_texture GL_OES_depth24 "
        "GL_OES_draw_texture GL_OES_framebuffer_object GL_OES_mapbuffer "
        "GL_OES_matrix_palette GL_OES_point_size_array GL_OES_point_sprite "
        "GL_OES_read_format GL_OES_rgb8_rgba8 "
        "GL_OES_texture_mirrored_repeat GL_APPLE_client_storage "
        "GL_APPLE_core_surface_texture GL_APPLE_texture_rectangle",
        2048,
        2048,
    };

    // The 7A341 SGX535 driver exposes both ES 1.1 and ES 2.0 strings. The HLE
    // implements the fixed-function ES 1.1 ABI today, so keep the ES 2.0 shader
    // capability private until that ABI is implemented instead of advertising a
    // path that would fail after context creation.
    constexpr OpenGlesGuestProfile sgx535 {
        "sgx535-fixed-function",
        common_vendor,
        sgx535_renderer,
        sgx535_version,
        "GL_EXT_texture_filter_anisotropic GL_EXT_texture_lod_bias "
        "GL_IMG_read_format GL_IMG_texture_compression_pvrtc "
        "GL_IMG_texture_format_BGRA8888 GL_OES_blend_subtract "
        "GL_OES_compressed_paletted_texture GL_OES_depth24 "
        "GL_OES_draw_texture GL_OES_framebuffer_object GL_OES_mapbuffer "
        "GL_OES_matrix_palette GL_OES_point_size_array GL_OES_point_sprite "
        "GL_OES_read_format GL_OES_rgb8_rgba8 "
        "GL_OES_texture_mirrored_repeat GL_APPLE_client_storage "
        "GL_APPLE_core_surface_texture GL_APPLE_texture_rectangle",
        2048,
        2048,
    };

    constexpr OpenGlesGuestProfile sgx535_framebuffer_objects {
        "sgx535-framebuffer-object",
        common_vendor,
        sgx535_renderer,
        sgx535_version,
        sgx535.extensions,
        sgx535.maximum_texture_dimension,
        sgx535.maximum_viewport_dimension,
    };

} // namespace

EaglContextProfileKind detect_eagl_context_profile(const UserlandHleCall& call)
{
    return call.symbol_address("-[EAGLContext GetMacroContextPrivate]")
               ? EaglContextProfileKind::FirmwareMacroDispatch
               : EaglContextProfileKind::HostManagedPublicAbi;
}

const OpenGlesGuestProfile& open_gles_guest_profile(
    OpenGlesGuestProfileKind kind)
{
    switch (kind) {
    case OpenGlesGuestProfileKind::MbxLiteLegacy:
        return legacy_mbx_lite;
    case OpenGlesGuestProfileKind::MbxLiteFramebufferObjects:
        return framebuffer_object_mbx_lite;
    case OpenGlesGuestProfileKind::Sgx535:
        return sgx535;
    case OpenGlesGuestProfileKind::Sgx535FramebufferObjects:
        return sgx535_framebuffer_objects;
    }
    return legacy_mbx_lite;
}

OpenGlesGuestProfileKind open_gles_framebuffer_profile(
    OpenGlesGuestProfileKind kind)
{
    switch (kind) {
    case OpenGlesGuestProfileKind::MbxLiteLegacy:
    case OpenGlesGuestProfileKind::MbxLiteFramebufferObjects:
        return OpenGlesGuestProfileKind::MbxLiteFramebufferObjects;
    case OpenGlesGuestProfileKind::Sgx535:
    case OpenGlesGuestProfileKind::Sgx535FramebufferObjects:
        return OpenGlesGuestProfileKind::Sgx535FramebufferObjects;
    }
    return OpenGlesGuestProfileKind::MbxLiteFramebufferObjects;
}

} // namespace ilemu
