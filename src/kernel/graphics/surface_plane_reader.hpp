// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#pragma once

#include "graphics/surface_plane.hpp"

#include <functional>
#include <memory>
#include <vector>

namespace ilemu {
class UserlandHleCall;
class UserlandHleRegistry;

// Read native CF containers through firmware functions, without depending on
// their private memory representation.
class SurfacePlaneReader
    : public std::enable_shared_from_this<SurfacePlaneReader> {
public:
    using Completion =
        std::function<void(UserlandHleCall&, std::vector<SurfacePlane>)>;
    static void register_symbols(UserlandHleRegistry& registry);
    static void read(UserlandHleCall& call, std::uint32_t dictionary,
        std::uint32_t number_output, Completion completion);

private:
    void next_plane(UserlandHleCall& call);
    void next_property(UserlandHleCall& call);
    std::uint32_t array_ { };
    std::uint32_t dictionary_ { };
    std::uint32_t number_output_ { };
    std::uint32_t count_ { };
    std::size_t property_ { };
    std::vector<SurfacePlane> planes_;
    Completion completion_;
};
} // namespace ilemu
