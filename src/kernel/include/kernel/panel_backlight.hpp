/* SPDX-License-Identifier: MPL-2.0 */
#pragma once

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <optional>
#include <ratio>

namespace ilemu {

// The panel applies the firmware's integer level and 16.16-second fade request.
// Clock values are Guest monotonic nanoseconds, independent of host rendering.
class PanelBacklight {
public:
    void set(std::uint8_t level, std::uint32_t fade_period, std::uint64_t now)
    {
        initial_ = sample(now);
        target_ = level;
        started_ = now;
        using FadePeriod = std::chrono::duration<std::uint32_t, std::ratio<1, 65536>>;
        finished_ = now + static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                FadePeriod { fade_period }).count());
        deadline_ = now;
    }

    [[nodiscard]] std::optional<std::uint64_t> next_deadline() const
    {
        return deadline_;
    }

    std::uint8_t advance(std::uint64_t now, std::uint64_t sample_period)
    {
        deadline_ = now < finished_
            ? std::optional { std::min(finished_, started_ +
                  ((now - started_) / sample_period + 1U) * sample_period) }
            : std::nullopt;
        return sample(now);
    }

private:
    [[nodiscard]] std::uint8_t sample(std::uint64_t now) const
    {
        if (now >= finished_)
            return target_;
        const auto fraction = static_cast<double>(now - started_) /
                              static_cast<double>(finished_ - started_);
        return static_cast<std::uint8_t>(std::lround(
            std::lerp(static_cast<double>(initial_),
                static_cast<double>(target_), fraction)));
    }

    std::uint8_t initial_ { 255 };
    std::uint8_t target_ { 255 };
    std::uint64_t started_ { };
    std::uint64_t finished_ { };
    std::optional<std::uint64_t> deadline_;
};

} // namespace ilemu
