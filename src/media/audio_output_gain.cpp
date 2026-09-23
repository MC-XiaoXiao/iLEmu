// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// Track firmware-programmed output gain independently of the host audio API.

#include "media/audio_output_gain.hpp"

#include <algorithm>
#include <cmath>
#include <utility>

namespace ilemu {

void AudioOutputGain::select_device(std::string_view uid)
{
    if (uid.empty())
        return;
    std::lock_guard lock { mutex_ };
    selected_device_ = uid;
}

void AudioOutputGain::set_hardware_volume(std::string_view uid, float gain)
{
    if (uid.empty() || !std::isfinite(gain))
        return;
    std::lock_guard lock { mutex_ };
    auto& device = devices_[std::string { uid }];
    device.hardware_gain = std::clamp(gain, 0.0F, 1.0F);
    device.reference_category = pending_category_;
    if (pending_category_) {
        const auto volume = category_volumes_.find(*pending_category_);
        device.reference_volume =
            volume == category_volumes_.end() ? 1.0F : volume->second;
    }
    if (!selected_device_)
        selected_device_ = uid;
}

void AudioOutputGain::set_hardware_mute(std::string_view uid, bool muted)
{
    if (uid.empty())
        return;
    std::lock_guard lock { mutex_ };
    devices_[std::string { uid }].muted = muted;
}

void AudioOutputGain::observe_category_volume(
    std::string_view category, float volume)
{
    if (category.empty() || !std::isfinite(volume))
        return;
    std::lock_guard lock { mutex_ };
    category_volumes_[std::string { category }] =
        std::clamp(volume, 0.0F, 1.0F);
    pending_category_ = category;
}

void AudioOutputGain::begin_stream()
{
    std::lock_guard lock { mutex_ };
    active_category_ = pending_category_;
}

float AudioOutputGain::gain() const
{
    std::lock_guard lock { mutex_ };
    if (!selected_device_)
        return 1.0F;
    const auto device = devices_.find(*selected_device_);
    if (device == devices_.end())
        return 1.0F;
    if (device->second.muted)
        return 0.0F;
    auto result = device->second.hardware_gain;
    if (active_category_ &&
        device->second.reference_category == active_category_) {
        const auto volume = category_volumes_.find(*active_category_);
        if (volume != category_volumes_.end()) {
            if (volume->second == 0.0F)
                return 0.0F;
            if (device->second.reference_volume > 0.0F)
                result *= volume->second / device->second.reference_volume;
        }
    }
    return std::clamp(result, 0.0F, 1.0F);
}

} // namespace ilemu
