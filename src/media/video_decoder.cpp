// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "media/video_decoder.hpp"

#include <atomic>

namespace ilemu {
namespace {
    std::atomic<H264DecoderFactory> decoder_factory;
}

void configure_h264_decoder(H264DecoderFactory factory)
{
    decoder_factory.store(factory);
}

H264DecoderFactory h264_decoder_factory() { return decoder_factory.load(); }
} // namespace ilemu
