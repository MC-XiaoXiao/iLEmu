// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "host/ffmpeg_video_decoder.hpp"

#include "media/video_decoder.hpp"

#if defined(ILEMU_HAS_FFMPEG)
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/mem.h>
#include <libswscale/swscale.h>
}

#include <array>
#include <cstring>
#include <limits>
#include <utility>

namespace ilemu {
namespace {
    void free_context(AVCodecContext* value) { avcodec_free_context(&value); }
    void free_frame(AVFrame* value) { av_frame_free(&value); }
    void free_packet(AVPacket* value) { av_packet_free(&value); }

    class FfmpegVideoDecoder final : public VideoDecoder {
    public:
        static std::unique_ptr<VideoDecoder> create(
            std::span<const std::byte> parameter_sets)
        {
            const auto* codec = avcodec_find_decoder(AV_CODEC_ID_H264);
            if (!codec ||
                parameter_sets.size() > std::numeric_limits<int>::max() -
                                            AV_INPUT_BUFFER_PADDING_SIZE)
                return { };
            auto decoder = std::make_unique<FfmpegVideoDecoder>(codec);
            if (!decoder->context_ || !decoder->frame_ || !decoder->packet_)
                return { };
            auto& context = *decoder->context_;
            context.thread_count = 1;
            context.max_pixels = 4096 * 4096;
            context.extradata = static_cast<std::uint8_t*>(av_mallocz(
                parameter_sets.size() + AV_INPUT_BUFFER_PADDING_SIZE));
            if (!context.extradata)
                return { };
            if (!parameter_sets.empty())
                std::memcpy(context.extradata, parameter_sets.data(),
                    parameter_sets.size());
            context.extradata_size = static_cast<int>(parameter_sets.size());
            if (avcodec_open2(&context, codec, nullptr) < 0)
                return { };
            return decoder;
        }

        explicit FfmpegVideoDecoder(const AVCodec* codec)
            : context_ { avcodec_alloc_context3(codec), free_context }
        {
        }

        std::optional<std::vector<DecodedVideoFrame>> decode(
            std::span<const std::byte> bytes, std::uint32_t identifier,
            bool display) override
        {
            const bool drain = bytes.empty();
            if (!drain) {
                if (bytes.size() > std::numeric_limits<int>::max() ||
                    av_new_packet(
                        packet_.get(), static_cast<int>(bytes.size())) < 0)
                    return std::nullopt;
                std::memcpy(packet_->data, bytes.data(), bytes.size());
                packet_->pts = (std::int64_t { display } << 32U) | identifier;
            }
            const auto sent = avcodec_send_packet(
                context_.get(), drain ? nullptr : packet_.get());
            av_packet_unref(packet_.get());
            if (sent < 0 && sent != AVERROR_EOF)
                return std::nullopt;
            std::vector<DecodedVideoFrame> result;
            while (true) {
                const auto received =
                    avcodec_receive_frame(context_.get(), frame_.get());
                if (received == AVERROR(EAGAIN) || received == AVERROR_EOF)
                    break;
                if (received < 0)
                    return std::nullopt;
                auto frame = export_frame(*frame_);
                av_frame_unref(frame_.get());
                if (!frame)
                    return std::nullopt;
                result.push_back(std::move(*frame));
            }
            if (drain)
                avcodec_flush_buffers(context_.get());
            return result;
        }

    private:
        std::optional<DecodedVideoFrame> export_frame(const AVFrame& source)
        {
            if (source.pts == AV_NOPTS_VALUE || source.width <= 0 ||
                source.height <= 0 || source.width > 4096 ||
                source.height > 4096)
                return std::nullopt;
            const auto width = static_cast<std::uint32_t>(source.width);
            const auto height = static_cast<std::uint32_t>(source.height);
            DecodedVideoFrame result { static_cast<std::uint32_t>(source.pts),
                width, height, (source.pts >> 32U) != 0, { } };
            if (!result.display)
                return result;
            scaler_.reset(sws_getCachedContext(scaler_.release(), source.width,
                source.height, static_cast<AVPixelFormat>(source.format),
                source.width, source.height, AV_PIX_FMT_BGRA, SWS_BILINEAR,
                nullptr, nullptr, nullptr));
            if (!scaler_)
                return std::nullopt;
            // Unknown matrix indices use libswscale's standard default.
            const auto* matrix = sws_getCoefficients(source.colorspace);
            if (sws_setColorspaceDetails(scaler_.get(), matrix,
                    source.color_range == AVCOL_RANGE_JPEG, matrix, 1, 0,
                    1 << 16, 1 << 16) < 0)
                return std::nullopt;
            result.bgra.resize(width * height * 4U);
            std::array<std::uint8_t*, 4> destination {
                reinterpret_cast<std::uint8_t*>(result.bgra.data())
            };
            const std::array<int, 4> strides { source.width * 4 };
            if (sws_scale(scaler_.get(), source.data, source.linesize, 0,
                    source.height, destination.data(),
                    strides.data()) != source.height)
                return std::nullopt;
            return result;
        }

        std::unique_ptr<AVCodecContext, decltype(&free_context)> context_;
        std::unique_ptr<AVFrame, decltype(&free_frame)> frame_ {
            av_frame_alloc(), free_frame
        };
        std::unique_ptr<AVPacket, decltype(&free_packet)> packet_ {
            av_packet_alloc(), free_packet
        };
        std::unique_ptr<SwsContext, decltype(&sws_freeContext)> scaler_ {
            nullptr, sws_freeContext
        };
    };
} // namespace
} // namespace ilemu
#endif

namespace ilemu {
void register_ffmpeg_video_decoder()
{
#if defined(ILEMU_HAS_FFMPEG)
    if (avcodec_find_decoder(AV_CODEC_ID_H264))
        configure_h264_decoder(&FfmpegVideoDecoder::create);
#endif
}
} // namespace ilemu
