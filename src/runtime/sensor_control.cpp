// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
#include "sensor_control.hpp"
#include "foundation/output.hpp"
#include "foundation/sensor_input.hpp"
#include <sstream>

namespace ilemu::runtime_detail {
void sensor_control(
    SensorInput& input, std::string_view command, Output& output)
{
    constexpr std::array names { "acceleration", "gyro", "magnetic" };
    std::istringstream parser { std::string { command } };
    std::string operation;
    parser >> operation;
    if (operation.empty() || operation == "status") {
        for (std::size_t i = 0; i < names.size(); ++i) {
            const auto kind = static_cast<MotionSensor>(i);
            const auto sample = input.motion(kind);
            std::ostringstream line;
            line << "[control] sensor " << names[i]
                 << " available=" << input.available(kind)
                 << " fresh=" << sample.has_value();
            if (sample)
                line << " xyz=" << sample->value[0] << ',' << sample->value[1]
                     << ',' << sample->value[2]
                     << " timestamp-ns=" << sample->timestamp_ns
                     << " accuracy=" << sample->accuracy;
            output.marker(line.str());
        }
        const auto fix = input.location();
        std::ostringstream line;
        line.precision(10);
        line << "[control] sensor location available=" << input.location_available()
             << " fresh=" << fix.has_value();
        if (fix)
            line << " latitude=" << fix->latitude
                 << " longitude=" << fix->longitude
                 << " accuracy-m=" << fix->horizontal_accuracy
                 << " unix-time=" << fix->unix_time_seconds;
        output.marker(line.str());
        return;
    }
    for (std::size_t i = 0; i < names.size(); ++i) {
        if (operation != names[i])
            continue;
        MotionSample sample;
        std::string trailing;
        if (!(parser >> sample.value[0] >> sample.value[1] >>
                sample.value[2]) ||
            parser >> trailing)
            break;
        sample.timestamp_ns =
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now().time_since_epoch())
                .count();
        const auto kind = static_cast<MotionSensor>(i);
        input.set_available(kind, true);
        output.marker(input.publish(kind, sample) ? "[control] sensor sample accepted"
                                                  : "[control] sensor sample rejected");
        return;
    }
    output.marker("[control] error: sensor [status|acceleration X Y Z|gyro X Y "
                  "Z|magnetic X Y Z]");
}
}
