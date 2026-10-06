// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// Sample the stationary virtual device in HID's gravity units.

#pragma once

#include "kernel/hid_event_queue.hpp"
#include "foundation/sensor_input.hpp"

namespace ilemu {

class HidAccelerometer {
public:
    void start(std::uint64_t now) { next_sample_ = now + sample_period; }
    void stop() { next_sample_.reset(); }

    [[nodiscard]] std::optional<std::uint64_t> next_deadline() const
    {
        return next_sample_;
    }

    [[nodiscard]] std::optional<HidEventQueue::Event> sample(
        std::uint64_t now, const SensorInput* input = nullptr)
    {
        if (!next_sample_ || now < *next_sample_)
            return std::nullopt;
        // Missed samples are not replayed in a burst. A real sensor with no
        // fresh reading must not be replaced with a fabricated stationary one.
        next_sample_ = now + sample_period;
        if (input && input->available(MotionSensor::Acceleration)) {
            next_sample_ = now + 20'000'000U;
            const auto sample = input->motion(MotionSensor::Acceleration);
            if (!sample) return std::nullopt;
            return HidEventQueue::Event { HidEventQueue::Acceleration {
                sample->value[0], sample->value[1], sample->value[2] }, now };
        }
        return HidEventQueue::Event {
            HidEventQueue::Acceleration { 0.0F, -1.0F, 0.0F }, now
        };
    }

private:
    // A low-rate hardware stream also serves clients that have not requested
    // a service-specific report interval. This is guest monotonic time in ns.
    static constexpr std::uint64_t sample_period = 100'000'000U;
    std::optional<std::uint64_t> next_sample_;
};

} // namespace ilemu
