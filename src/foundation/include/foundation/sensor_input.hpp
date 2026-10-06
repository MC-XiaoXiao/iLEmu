// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
#pragma once

#include <array>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <optional>

namespace ilemu {

enum class MotionSensor : std::uint8_t {
    Acceleration,
    AngularVelocity,
    MagneticField,
    Count
};

// Device coordinates: +X right, +Y towards the top, +Z out of the screen.
// Acceleration includes gravity in g (resting face up: 0,0,-1), angular
// velocity is rad/s, magnetic field is microtesla. No host SDK types escape.
struct MotionSample {
    std::array<float, 3> value { };
    std::uint64_t timestamp_ns { };
    int accuracy { -1 };
};

struct LocationSample {
    double latitude { };
    double longitude { };
    double horizontal_accuracy { };
    double unix_time_seconds { };
    std::uint64_t timestamp_ns { };
    std::optional<double> altitude;
    std::optional<double> vertical_accuracy;
    std::optional<double> speed;
    std::optional<double> bearing;
};

// One session's bounded latest-value mailbox. Producers may run on host
// callbacks; guest consumers never call a host API or wait for new samples.
class SensorInput {
public:
    void reset();
    void set_available(MotionSensor sensor, bool available);
    [[nodiscard]] bool available(MotionSensor sensor) const;
    bool publish(MotionSensor sensor, MotionSample sample);
    [[nodiscard]] std::optional<MotionSample> motion(MotionSensor sensor) const;
    void set_location_available(bool available);
    [[nodiscard]] bool location_available() const;
    bool publish(LocationSample sample);
    [[nodiscard]] std::optional<LocationSample> location() const;

private:
    using Clock = std::chrono::steady_clock;
    template <class T> struct Slot {
        bool available { };
        std::optional<T> sample;
        Clock::time_point received { };
    };
    mutable std::mutex mutex_;
    std::array<Slot<MotionSample>,
        static_cast<std::size_t>(MotionSensor::Count)>
        motion_;
    Slot<LocationSample> location_;
};

} // namespace ilemu
