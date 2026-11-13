/* SPDX-License-Identifier: MPL-2.0 */
#pragma once

#include "kernel/hid_event_queue.hpp"
#include <mutex>

namespace ilemu {

// Firmware controls the sensor's continuous mode and report period. Brightness
// policy belongs to its HID session filters, not this virtual light source.
class HidAmbientLightSensor {
public:
    void open(std::uint64_t now)
    {
        std::lock_guard lock { mutex_ };
        open_ = true;
        schedule(now);
    }
    void close()
    {
        std::lock_guard lock { mutex_ };
        open_ = false;
        next_sample_.reset();
    }
    void set_continuous(bool enabled, std::uint64_t now)
    {
        std::lock_guard lock { mutex_ };
        continuous_ = enabled;
        schedule(now);
    }
    void set_report_interval(std::uint32_t microseconds, std::uint64_t now)
    {
        std::lock_guard lock { mutex_ };
        period_ = static_cast<std::uint64_t>(microseconds) * 1'000U;
        schedule(now);
    }
    [[nodiscard]] std::optional<std::uint64_t> next_deadline() const
    {
        std::lock_guard lock { mutex_ };
        return next_sample_;
    }
    std::optional<HidEventQueue::Event> sample(
        std::uint64_t now, std::uint32_t lux)
    {
        std::lock_guard lock { mutex_ };
        if (!next_sample_ || now < *next_sample_)
            return std::nullopt;
        next_sample_ = now + period_;
        return HidEventQueue::Event { HidEventQueue::AmbientLight { lux },
            now };
    }

private:
    void schedule(std::uint64_t now)
    {
        next_sample_ = open_ && continuous_ && period_ != 0U
                           ? std::optional { now }
                           : std::nullopt;
    }
    mutable std::mutex mutex_;
    bool open_ { };
    bool continuous_ { };
    std::uint64_t period_ { 100'000'000U };
    std::optional<std::uint64_t> next_sample_;
};

} // namespace ilemu
