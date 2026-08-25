#pragma once

#include <memory>

#include "ilemu/audio.hpp"

namespace ilemu {

class FfmpegAudioDecoder final : public AudioDecoder {
public:
    FfmpegAudioDecoder();
    ~FfmpegAudioDecoder() override;

    FfmpegAudioDecoder(const FfmpegAudioDecoder&) = delete;
    FfmpegAudioDecoder& operator=(const FfmpegAudioDecoder&) = delete;

    [[nodiscard]] static bool available();
    [[nodiscard]] std::optional<AudioBuffer> decode(
        const std::filesystem::path& path) override;
    [[nodiscard]] std::string last_error() const override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace ilemu
