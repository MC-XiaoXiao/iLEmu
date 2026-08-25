#pragma once

#include <cstdint>
#include <string_view>

namespace ilemu {

enum class OpenGlesGuestProfileKind {
    MbxLiteLegacy,
    MbxLiteFramebufferObjects,
    Sgx535,
    Sgx535FramebufferObjects,
};

// Guest-visible capabilities of the firmware GPU driver. Host renderer names
// and limits never cross this boundary: UIKit and QuartzCore use these values
// to select paths supported by the emulated device.
struct OpenGlesGuestProfile {
    std::string_view name;
    std::string_view vendor;
    std::string_view renderer;
    std::string_view version;
    std::string_view extensions;
    std::uint32_t maximum_texture_dimension;
    std::uint32_t maximum_viewport_dimension;
};

[[nodiscard]] const OpenGlesGuestProfile& open_gles_guest_profile(
    OpenGlesGuestProfileKind kind);

[[nodiscard]] OpenGlesGuestProfileKind open_gles_framebuffer_profile(
    OpenGlesGuestProfileKind kind);

} // namespace ilemu
