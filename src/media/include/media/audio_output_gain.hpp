// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// Combine firmware audio-category volume with virtual output-device controls.

#pragma once

#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>

namespace ilemu {

// The firmware owns volume policy and programs the virtual hardware. Its
// category reply arrives immediately, while a hardware volume write may wait
// until the next output-route setup. Keep that interim adjustment relative to
// the firmware's last programmed hardware level.
class AudioOutputGain final {
public:
    void select_device(std::string_view uid);
    void set_hardware_volume(std::string_view uid, float gain);
    void set_hardware_mute(std::string_view uid, bool muted);
    void observe_category_volume(std::string_view category, float volume);
    void begin_stream();
    [[nodiscard]] float gain() const;

private:
    struct DeviceState {
        float hardware_gain { 1.0F };
        bool muted { };
        std::optional<std::string> reference_category;
        float reference_volume { 1.0F };
    };

    mutable std::mutex mutex_;
    std::map<std::string, DeviceState, std::less<>> devices_;
    std::map<std::string, float, std::less<>> category_volumes_;
    std::optional<std::string> selected_device_;
    std::optional<std::string> pending_category_;
    std::optional<std::string> active_category_;
};

} // namespace ilemu
