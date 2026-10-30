// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#pragma once

#include <string_view>

namespace ilemu {

struct BuildInfo {
    std::string_view version;
    std::string_view timestamp;
    std::string_view user;

    [[nodiscard]] static const BuildInfo& current();
};

} // namespace ilemu
