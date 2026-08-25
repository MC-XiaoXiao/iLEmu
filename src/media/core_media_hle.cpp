#include "ilemu/core_media_hle.hpp"

#include <string>
#include <string_view>

#include "ilemu/userland_hle.hpp"

namespace ilemu {
namespace {

    constexpr std::string_view core_media_image {
        "/System/Library/PrivateFrameworks/CoreMedia.framework/CoreMedia"
    };
} // namespace

CoreMediaHle::CoreMediaHle(UserlandHleRegistry& registry)
{
    registry.register_function(std::string { core_media_image },
        "_CMSessionMgrCopyDeviceRouteForAudioCategory",
        &CoreMediaHle::copy_device_route_for_audio_category);
}

void CoreMediaHle::copy_device_route_for_audio_category(UserlandHleCall& call)
{
    call.resume_original_persistently();
}

} // namespace ilemu
