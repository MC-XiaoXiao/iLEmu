// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
#include "foundation/sensor_input.hpp"

#include <algorithm>
#include <cmath>

namespace ilemu {

void SensorInput::reset()
{
    const std::lock_guard lock { mutex_ };
    motion_ = { };
    location_ = { };
}

void SensorInput::set_available(MotionSensor sensor, bool available)
{
    const auto index = static_cast<std::size_t>(sensor);
    if (index >= motion_.size())
        return;
    const std::lock_guard lock { mutex_ };
    auto& slot = motion_[index];
    slot.available = available;
    if (!available)
        slot.sample.reset();
}

bool SensorInput::available(MotionSensor sensor) const
{
    const auto index = static_cast<std::size_t>(sensor);
    const std::lock_guard lock { mutex_ };
    return index < motion_.size() && motion_[index].available;
}

bool SensorInput::publish(MotionSensor sensor, MotionSample sample)
{
    const auto index = static_cast<std::size_t>(sensor);
    if (index >= motion_.size() || sample.timestamp_ns == 0 ||
        !std::all_of(sample.value.begin(), sample.value.end(),
            [](float value) { return std::isfinite(value); }))
        return false;
    const std::lock_guard lock { mutex_ };
    auto& slot = motion_[index];
    if (!slot.available ||
        (slot.sample && sample.timestamp_ns <= slot.sample->timestamp_ns))
        return false;
    slot.sample = sample;
    slot.received = Clock::now();
    return true;
}

std::optional<MotionSample> SensorInput::motion(MotionSensor sensor) const
{
    const auto index = static_cast<std::size_t>(sensor);
    const std::lock_guard lock { mutex_ };
    if (index >= motion_.size())
        return std::nullopt;
    const auto& slot = motion_[index];
    if (!slot.available ||
        Clock::now() - slot.received > std::chrono::seconds { 2 })
        return std::nullopt;
    return slot.sample;
}

void SensorInput::set_location_available(bool available)
{
    const std::lock_guard lock { mutex_ };
    location_.available = available;
    if (!available)
        location_.sample.reset();
}

bool SensorInput::location_available() const
{
    const std::lock_guard lock { mutex_ };
    return location_.available;
}

bool SensorInput::publish(LocationSample sample)
{
    const auto finite = [](const std::optional<double>& value) {
        return !value || std::isfinite(*value);
    };
    if (!std::isfinite(sample.latitude) || std::abs(sample.latitude) > 90 ||
        !std::isfinite(sample.longitude) || std::abs(sample.longitude) > 180 ||
        !std::isfinite(sample.horizontal_accuracy) ||
        sample.horizontal_accuracy < 0 ||
        !std::isfinite(sample.unix_time_seconds) ||
        sample.unix_time_seconds <= 0 || sample.timestamp_ns == 0 ||
        !finite(sample.altitude) || !finite(sample.vertical_accuracy) ||
        !finite(sample.speed) || !finite(sample.bearing) ||
        (sample.vertical_accuracy && *sample.vertical_accuracy < 0) ||
        (sample.speed && *sample.speed < 0) ||
        (sample.bearing && (*sample.bearing < 0 || *sample.bearing >= 360)))
        return false;
    const std::lock_guard lock { mutex_ };
    if (!location_.available ||
        (location_.sample &&
            sample.timestamp_ns <= location_.sample->timestamp_ns))
        return false;
    location_.sample = sample;
    location_.received = Clock::now();
    return true;
}

std::optional<LocationSample> SensorInput::location() const
{
    const std::lock_guard lock { mutex_ };
    if (!location_.available ||
        Clock::now() - location_.received > std::chrono::seconds { 30 })
        return std::nullopt;
    return location_.sample;
}

} // namespace ilemu
