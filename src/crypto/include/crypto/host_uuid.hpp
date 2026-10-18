// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#pragma once

#include <string>

namespace ilemu {

// A fresh canonical UUID from the host platform's UUID implementation.
[[nodiscard]] std::string make_host_uuid();

} // namespace ilemu
