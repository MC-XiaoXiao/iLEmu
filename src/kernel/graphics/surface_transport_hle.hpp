// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#pragma once

#include <cstdint>
#include <limits>
#include <optional>

#include "foundation/userland_hle.hpp"
#include "graphics/surface_store.hpp"
#include "graphics/surface_transport_abi.hpp"

namespace ilemu::surface_transport {

[[nodiscard]] inline Kind io_surface_kind(UserlandHleCall& call)
{
    const auto width = call.original_function_code("_IOSurfaceClientGetWidth", 8);
    const auto height = call.original_function_code("_IOSurfaceClientGetHeight", 8);
    return width && height ? io_surface_kind(*width, *height)
                           : Kind::IOSurfaceClient;
}

[[nodiscard]] inline const ClientAbi& loaded_client_abi(UserlandHleCall& call)
{
    return call.image_loaded(io_surface_client.image_suffix)
               ? for_kind(io_surface_kind(call))
               : core_surface_client_buffer;
}

[[nodiscard]] inline std::optional<std::uint32_t> public_surface_identifier(
    UserlandHleCall& call, const SurfaceStore& surfaces,
    std::uint32_t public_surface)
{
    if (public_surface == 0)
        return std::nullopt;
    const auto& profile = loaded_client_abi(call);
    for (const auto pointer_offset : profile.public_client_pointer_offsets) {
        if (pointer_offset == 0 ||
            public_surface > std::numeric_limits<std::uint32_t>::max() -
                                 pointer_offset)
            continue;
        const auto client =
            call.memory().read32(public_surface + pointer_offset);
        if (!client || *client == 0 ||
            *client > std::numeric_limits<std::uint32_t>::max() -
                          profile.identifier_offset)
            continue;
        const auto identifier =
            call.memory().read32(*client + profile.identifier_offset);
        if (identifier && *identifier != 0 && surfaces.find(*identifier))
            return identifier;
    }
    return std::nullopt;
}

} // namespace ilemu::surface_transport
