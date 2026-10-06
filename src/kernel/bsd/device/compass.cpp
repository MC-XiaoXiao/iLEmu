// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
#include "kernel/compass_device.hpp"
#include <algorithm>
#include <bit>
#include <cmath>

namespace ilemu {

CompassDevice::Parameters CompassDevice::parameters() const
{
    const std::lock_guard lock { mutex_ };
    return parameters_;
}

bool CompassDevice::configure(const Parameters& parameters)
{
    const auto mask = parameters[0];
    if ((mask & ~7U) != 0)
        return false;
    if ((mask & 1U) && parameters[1] > 60'000'000U)
        return false;
    for (std::size_t i = 4; (mask & 4U) && i < 7; ++i)
        if (parameters[i] < 1 || parameters[i] > 100U * 65536U)
            return false;
    const std::lock_guard lock { mutex_ };
    if (mask & 1U) {
        // A zero period powers the sensor down. In particular, firmware opens
        // the descriptor before initializing its calibration state.
        interval_ns_ = parameters[1] == 0 ? 0
            : std::max<std::uint64_t>(20'000, parameters[1]) * 1000;
        position_ = packet_.size();
        next_sample_ = 0;
    }
    if (mask & 2U) {
        // The wire structure packs three uint16 DAC codes at byte offset 8;
        // offset 4 is the sampling period, and gains begin at offset 16.
        parameters_[2] = parameters[2];
        parameters_[3] = parameters[3] & 0xffffU;
    }
    if (mask & 4U)
        std::copy_n(parameters.begin() + 4, 3, parameters_.begin() + 4);
    return true;
}

void CompassDevice::sample(const SensorInput& input, std::uint64_t now)
{
    if (interval_ns_ == 0 || position_ < packet_.size() || now < next_sample_)
        return;
    const auto field = input.motion(MotionSensor::MagneticField);
    if (!field || field->timestamp_ns == source_timestamp_)
        return;
    source_timestamp_ = field->timestamp_ns;
    next_sample_ = now + interval_ns_;
    const auto& settings = parameters_;
    std::array<std::uint32_t, 4> raw { };
    for (std::size_t axis = 0; axis < 3; ++axis) {
        // The driver's record carries unsigned ADC codes in 16.16 words;
        // the firmware converts its 128 midpoint to signed magnetic values.
        const auto dac = (settings[2 + axis / 2] >> (16 * (axis % 2))) & 0xffU;
        // AKM's sign/magnitude DAC shifts sixteen ADC counts per step.
        // Firmware's offset search uses that same transfer function.
        const auto offset = dac <= 127 ? -static_cast<int>(dac)
                                      : static_cast<int>(dac) - 128;
        const auto value = 128.0 + field->value[axis] + 16.0 * offset;
        raw[axis] =
            static_cast<std::uint32_t>(std::lround(std::clamp(value, 0.0, 255.0)))
            << 16;
    }
    raw[3] = 120U << 16; // Nominal sensor temperature ADC code.
    for (std::size_t i = 0; i < raw.size(); ++i)
        for (std::size_t byte = 0; byte < 4; ++byte)
            packet_[i * 4 + byte] =
                static_cast<std::byte>(raw[i] >> (byte * 8));
    position_ = 0;
}

std::size_t CompassDevice::pending_bytes(
    const SensorInput& input, std::uint64_t now)
{
    const std::lock_guard lock { mutex_ };
    sample(input, now);
    return packet_.size() - position_;
}

std::vector<std::byte> CompassDevice::read(
    const SensorInput& input, std::uint64_t now, std::size_t maximum)
{
    const std::lock_guard lock { mutex_ };
    sample(input, now);
    const auto count = std::min(maximum, packet_.size() - position_);
    const auto bytes = std::span { packet_ }.subspan(position_, count);
    std::vector<std::byte> result(bytes.begin(), bytes.end());
    position_ += count;
    return result;
}
}
