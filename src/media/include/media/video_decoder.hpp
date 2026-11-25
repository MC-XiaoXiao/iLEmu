// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <vector>

namespace ilemu {

struct DecodedVideoFrame {
    std::uint32_t identifier;
    std::uint32_t width;
    std::uint32_t height;
    std::vector<std::byte> bgra;
};

class VideoDecoder {
public:
    virtual ~VideoDecoder() = default;
    // Input is an Annex B access unit. An empty input drains delayed frames.
    // Frames retain the identifiers of the access units that produced them.
    [[nodiscard]] virtual std::optional<std::vector<DecodedVideoFrame>> decode(
        std::span<const std::byte> bytes, std::uint32_t identifier) = 0;
};

using H264DecoderFactory = std::unique_ptr<VideoDecoder> (*)(
    std::span<const std::byte> parameter_sets);

void configure_h264_decoder(H264DecoderFactory factory);
[[nodiscard]] H264DecoderFactory h264_decoder_factory();

} // namespace ilemu
