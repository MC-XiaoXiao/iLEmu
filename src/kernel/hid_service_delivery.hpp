// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#pragma once

#include <cstdint>
#include <optional>

namespace ilemu {

class UserlandHleCall;
class UserlandHleRegistry;

// The firmware owns the service queue, its QoS and the downstream routing.
// A native CFArray retains the service and event until dispatch invokes us.
class HidServiceDelivery {
public:
    static void register_functions(UserlandHleRegistry& registry);
    [[nodiscard]] static bool available(const UserlandHleCall& call);
    [[nodiscard]] static std::optional<std::uint32_t> callback(
        UserlandHleCall& call);
};

} // namespace ilemu
