// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
#include "android_host.hpp"

#include <cerrno>
#include <fcntl.h>
#include <sys/stat.h>
#include <system_error>
#include <unistd.h>

#include "app/live_control.hpp"
#include "app/sdl_audio_sink.hpp"
#include "app/sdl_display.hpp"
#include "foundation/device_model.hpp"
#include "host/native_gles.hpp"
#include "host/resource_usage.hpp"

namespace ilemu {

AndroidHost::AndroidHost(
    std::filesystem::path control_path, const std::atomic<bool>& stop)
    : control_path_ { std::move(control_path) }
    , stop_ { stop }
{
    // Private app storage restricts this channel to the app and adb run-as in
    // debuggable builds. RDWR keeps the parser alive between command writers.
    ::unlink(control_path_.c_str());
    if (::mkfifo(control_path_.c_str(), 0600) != 0)
        throw std::system_error(errno, std::generic_category(), "control fifo");
    control_fd_ =
        ::open(control_path_.c_str(), O_RDWR | O_NONBLOCK | O_CLOEXEC);
    if (control_fd_ < 0) {
        const auto error = errno;
        ::unlink(control_path_.c_str());
        throw std::system_error(error, std::generic_category(), "open control");
    }
}

AndroidHost::~AndroidHost()
{
    if (control_fd_ >= 0)
        ::close(control_fd_);
    ::unlink(control_path_.c_str());
}

bool AndroidHost::stop_requested() const { return stop_.load(); }

void AndroidHost::initialize_graphics() { register_native_gles_renderer(); }

std::unique_ptr<DisplayPresenter> AndroidHost::create_display(
    const DeviceModel& device)
{
    return std::make_unique<SdlDisplay>(
        device.screen.panel, device.screen.user_interface);
}

std::unique_ptr<ControlChannel> AndroidHost::create_control(
    const DeviceModel& device)
{
    return std::make_unique<LiveControl>(control_fd_,
        device.screen.user_interface, device.input.system_gestures);
}

SessionAudio AndroidHost::create_audio()
{
    SessionAudio audio;
    audio.sink = std::make_shared<SdlAudioSink>();
    audio.backend_name = "sdl";
    return audio;
}

HostMemorySnapshot AndroidHost::memory_snapshot() const
{
    return host_memory_snapshot();
}
HostMemoryBudgetSnapshot AndroidHost::memory_budget_snapshot() const
{
    return host_memory_budget_snapshot();
}

} // namespace ilemu
