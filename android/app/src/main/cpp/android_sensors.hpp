// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
#pragma once
#include "foundation/sensor_input.hpp"
#include <memory>

namespace ilemu {
std::shared_ptr<SensorInput> android_sensors();
}
