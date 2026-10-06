// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
#pragma once
#include <string_view>
namespace ilemu {
class SensorInput;
class Output;
namespace runtime_detail {
    void sensor_control(
        SensorInput& input, std::string_view command, Output& output);
}
}
