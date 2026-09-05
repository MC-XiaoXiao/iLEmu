#pragma once

#include <cstdint>
#include <string_view>

namespace ilemu {

class UserlandHleCall;

enum class EaglContextAbi {
    HostManagedPublicAbi,
    FirmwareMacroDispatch,
};

[[nodiscard]] EaglContextAbi detect_eagl_context_abi(
    const UserlandHleCall& call);

enum class OpenGlesGuestCapabilitySet {
    MbxLiteLegacy,
    MbxLiteFramebufferObjects,
    Sgx535,
    Sgx535FramebufferObjects,
};

// Guest-visible capabilities of the firmware GPU driver. Host renderer names
// and limits never cross this boundary: UIKit and QuartzCore use these values
// to select paths supported by the emulated device.
struct OpenGlesGuestCapabilities {
    std::string_view name;
    std::string_view vendor;
    std::string_view renderer;
    std::string_view version;
    std::string_view extensions;
    std::uint32_t maximum_texture_dimension;
    std::uint32_t maximum_viewport_dimension;
};

[[nodiscard]] const OpenGlesGuestCapabilities& open_gles_guest_capabilities(
    OpenGlesGuestCapabilitySet kind);

[[nodiscard]] OpenGlesGuestCapabilitySet open_gles_framebuffer_capabilities(
    OpenGlesGuestCapabilitySet kind);

} // namespace ilemu
