#include "app/desktop_host.hpp"

#include <stdexcept>

#include "app/live_control.hpp"
#include "foundation/device_profile.hpp"
#include "host/ffmpeg_audio_decoder.hpp"
#include "host/native_gles.hpp"
#include "host/resource_usage.hpp"
#include "app/sdl_audio_sink.hpp"
#include "app/sdl_display.hpp"

namespace ilemu {

void DesktopHost::initialize_graphics() { register_native_gles_renderer(); }

std::unique_ptr<DisplayPresenter> DesktopHost::create_display(
    const DeviceProfile& device)
{
    if (!SdlDisplay::available()) {
        throw std::runtime_error {
            "--display sdl requested, but SDL2 support is not built"
        };
    }
    return std::make_unique<SdlDisplay>(device.display, device.user_interface);
}

std::unique_ptr<ControlChannel> DesktopHost::create_control(
    const DeviceProfile& device)
{
    return std::make_unique<LiveControl>(
        0, device.user_interface, device.system_gestures);
}

SessionAudio DesktopHost::create_audio()
{
    SessionAudio result;
    if (SdlAudioSink::available()) {
        result.sink = std::make_shared<SdlAudioSink>();
        result.backend_name = "sdl";
    }
    if (FfmpegAudioDecoder::available()) {
        result.decoder = std::make_shared<FfmpegAudioDecoder>();
        result.decoder_name = "ffmpeg";
    }
    return result;
}

HostMemorySnapshot DesktopHost::memory_snapshot() const
{
    return host_memory_snapshot();
}

HostMemoryBudgetSnapshot DesktopHost::memory_budget_snapshot() const
{
    return host_memory_budget_snapshot();
}

} // namespace ilemu
