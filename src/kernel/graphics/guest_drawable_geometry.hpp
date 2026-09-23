// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string_view>

#include "foundation/display_geometry.hpp"

namespace ilemu {

class UserlandHleCall;

struct GuestDrawableGeometry {
    DisplayGeometry bounds;
    DisplayGeometry pixels;
};

// Ask the firmware's CA drawable for its bounds and contentsScale. The
// Objective-C accessors and object boxing stay in guest code; this adapter
// only reads the values needed to allocate a host renderbuffer.
class GuestDrawableGeometryReader final
    : public std::enable_shared_from_this<GuestDrawableGeometryReader> {
public:
    using Completion = std::function<void(UserlandHleCall&,
        std::optional<GuestDrawableGeometry>)>;

    static void read(UserlandHleCall& call, std::uint32_t drawable,
        Completion completion);

private:
    using ValueCompletion =
        std::function<void(UserlandHleCall&, std::uint32_t)>;

    GuestDrawableGeometryReader(std::uint32_t drawable, Completion completion);

    void read_bounds(UserlandHleCall& call);
    void read_scale(UserlandHleCall& call);
    void complete(UserlandHleCall& call,
        std::optional<GuestDrawableGeometry> geometry);
    void property(UserlandHleCall& call, std::string_view name,
        ValueCompletion completion);
    void message(UserlandHleCall& call, std::uint32_t object,
        std::string_view selector, std::uint32_t argument,
        ValueCompletion completion);

    std::uint32_t drawable_ { };
    DisplayGeometry bounds_;
    Completion completion_;
};

} // namespace ilemu
