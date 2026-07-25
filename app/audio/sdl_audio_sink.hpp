#pragma once

#include <memory>
#include <mutex>
#include <string>

#include "ilemu/audio.hpp"

namespace ilemu {

class SdlAudioSink final : public AudioSink {
public:
  SdlAudioSink();
  ~SdlAudioSink() override;

  SdlAudioSink(const SdlAudioSink &) = delete;
  SdlAudioSink &operator=(const SdlAudioSink &) = delete;

  [[nodiscard]] static bool available();
  [[nodiscard]] bool play(const AudioBuffer &buffer) override;
  [[nodiscard]] bool has_pending_audio() const override;
  void set_gain(float gain) override;
  void stop(AudioStopMode mode = AudioStopMode::Immediate) override;
  [[nodiscard]] std::string last_error() const override;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace ilemu
