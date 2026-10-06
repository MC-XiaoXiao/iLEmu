// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
#pragma once
#include "foundation/sensor_input.hpp"
#include <array>
#include <mutex>
#include <span>
#include <string_view>
#include <vector>

namespace ilemu {

// AK8973 character-device protocol. Firmware retains its own offset search,
// calibration, tilt compensation and magnetic-declination model.
class CompassDevice {
public:
    static constexpr std::string_view path = "/dev/compass_0";
    static constexpr std::string_view descriptor_kind = "compass";
    static constexpr std::uint32_t device_minor = 6;
    static constexpr std::uint32_t set_parameters = 0xc01c5466;
    static constexpr std::uint32_t get_parameters = 0xc01c5467;
    using Parameters = std::array<std::uint32_t, 7>;

    [[nodiscard]] Parameters parameters() const;
    bool configure(const Parameters& parameters);
    [[nodiscard]] std::size_t pending_bytes(
        const SensorInput& input, std::uint64_t now);
    std::vector<std::byte> read(
        const SensorInput& input, std::uint64_t now, std::size_t maximum);

private:
    void sample(const SensorInput& input, std::uint64_t now);
    mutable std::mutex mutex_;
    Parameters parameters_ { 0, 0, 0, 0, 1756365, 1756365, 1756365 };
    std::uint64_t interval_ns_ { };
    std::uint64_t next_sample_ { };
    std::uint64_t source_timestamp_ { };
    std::array<std::byte, 16> packet_ { };
    std::size_t position_ { 16 };
};
}
