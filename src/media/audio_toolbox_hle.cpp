#include "ilemu/audio_toolbox_hle.hpp"

#include <string>
#include <string_view>

#include "ilemu/output.hpp"
#include "ilemu/userland_hle.hpp"

namespace ilemu {
namespace {

constexpr std::string_view audio_toolbox_image{
    "/System/Library/Frameworks/AudioToolbox.framework/AudioToolbox"};

} // namespace

AudioToolboxHle::AudioToolboxHle(UserlandHleRegistry &registry) {
  const auto register_play = [&](std::string symbol) {
    registry.register_function(
        std::string{audio_toolbox_image}, std::move(symbol),
        [this](UserlandHleCall &call) { play_system_sound(call); });
  };
  register_play("_AudioServicesPlaySystemSound");
  register_play("_AudioServicesPlayInterfaceSound");
  register_play("_AudioServicesPlayAlertSound");
}

void AudioToolboxHle::play_system_sound(UserlandHleCall &call) {
  const auto sound_id = call.argument(0);
  call.output().line("[audio] system-sound pid=" +
                     std::to_string(call.process_id()) + " id=" +
                     std::to_string(sound_id) + " service=native");
  // The firmware service owns the system-sound registry and resource
  // selection. Keeping this boundary native avoids duplicating either IDs or
  // paths in the emulator as new system sounds appear.
  call.resume_original();
}

} // namespace ilemu
