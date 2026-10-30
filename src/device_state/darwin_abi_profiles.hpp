// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
#pragma once

#include "device_state/darwin_kernel_configuration.hpp"

namespace ilemu {

[[nodiscard]] const DarwinConfigurationEntry* darwin_configuration_for_build(
    DarwinBuildVersion build);

} // namespace ilemu
