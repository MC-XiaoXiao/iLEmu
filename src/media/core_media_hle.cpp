// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// Register the CoreMedia audio-route and native decoder adapters.

#include "media/core_media_hle.hpp"
#include "media/vxd390_video_hle.hpp"

#include <string>
#include <string_view>

#include "foundation/userland_hle.hpp"

namespace ilemu {
namespace {

    constexpr std::string_view core_media_image {
        "/System/Library/PrivateFrameworks/CoreMedia.framework/CoreMedia"
    };
} // namespace

CoreMediaHle::CoreMediaHle(UserlandHleRegistry& registry)
{
    register_vxd390_video_hle(registry);
    registry.register_function(std::string { core_media_image },
        "_CMSessionMgrCopyDeviceRouteForAudioCategory",
        &CoreMediaHle::copy_device_route_for_audio_category);
}

void CoreMediaHle::copy_device_route_for_audio_category(UserlandHleCall& call)
{
    call.resume_original_persistently();
}

} // namespace ilemu
