// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "media/vxd390_video_hle.hpp"

#include "foundation/address_space.hpp"
#include "foundation/cpu.hpp"
#include "foundation/output.hpp"
#include "foundation/userland_hle.hpp"
#include "media/video_decoder.hpp"

#include <algorithm>
#include <array>
#include <initializer_list>
#include <limits>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <string_view>
#include <utility>

namespace ilemu {
namespace {
    constexpr std::string_view h264_plugin {
        "/VideoDecoders/H264H4.videodecoder"
    };
    constexpr std::string_view vxd_image {
        "/AppleVXD390Framework.framework/AppleVXD390Framework"
    };
    constexpr std::uint32_t bad_argument = 0xe00002c2U;
    constexpr std::uint32_t no_memory = 0xe00002bdU;
    constexpr std::uint32_t unsupported = 0xe00002c7U;
    constexpr std::uint32_t video_decode_failed =
        static_cast<std::uint32_t>(-12909);
    constexpr std::uint32_t maximum_access_unit = 16U * 1024U * 1024U;
    constexpr std::uint32_t maximum_dimension = 4096U;
    constexpr auto pixel_format_bgra = 0x42475241U;
    constexpr auto pixel_attributes = "_CreatePixelBufferAttributesDictionary";
    constexpr std::array core_video_functions {
        "_CVPixelBufferPoolCreatePixelBuffer", "_CVPixelBufferLockBaseAddress",
        "_CVPixelBufferGetPixelFormatType", "_CVPixelBufferGetBaseAddress",
        "_CVPixelBufferGetBytesPerRow", "_CVPixelBufferUnlockBaseAddress"
    };

    void arguments(
        UserlandHleCall& call, std::initializer_list<std::uint32_t> values)
    {
        std::copy(values.begin(), values.end(), call.cpu().registers().begin());
    }

    // The firmware repackages avcC parameter sets and access units as
    // length-prefixed NAL units. The decoder interface takes Annex B.
    std::optional<std::vector<std::byte>> annex_b(
        std::span<const std::byte> bytes, std::uint32_t length_size)
    {
        if (length_size != 1U && length_size != 2U && length_size != 4U)
            return std::nullopt;
        constexpr std::array start_code { std::byte { 0 }, std::byte { 0 },
            std::byte { 0 }, std::byte { 1 } };
        std::vector<std::byte> output;
        while (!bytes.empty()) {
            if (bytes.size() < length_size)
                return std::nullopt;
            std::uint32_t size { };
            for (const auto byte : bytes.first(length_size))
                size = (size << 8U) | std::to_integer<std::uint32_t>(byte);
            bytes = bytes.subspan(length_size);
            if (size == 0U || size > bytes.size() ||
                output.size() + start_code.size() + size > maximum_access_unit)
                return std::nullopt;
            output.insert(output.end(), start_code.begin(), start_code.end());
            output.insert(output.end(), bytes.begin(), bytes.begin() + size);
            bytes = bytes.subspan(size);
        }
        return output;
    }

    struct DecoderSession {
        std::unique_ptr<VideoDecoder> decoder;
        std::uint32_t width { };
        std::uint32_t height { };
        std::uint32_t length_size { };
        std::uint32_t callback { };
        std::uint32_t callback_context { };
        std::uint32_t pool { };
        std::uint32_t scratch { };
        std::uint32_t bitstream { };
        std::uint32_t bitstream_capacity { };
        bool reported_output { };
    };

    // CoreVideo allocates and owns an IOSurface-backed pixel buffer, so
    // VideoToolbox can hand it to the native presenter with ordinary lifetime
    // and synchronization semantics.
    class FrameDelivery : public std::enable_shared_from_this<FrameDelivery> {
    public:
        FrameDelivery(std::shared_ptr<DecoderSession> session,
            std::vector<DecodedVideoFrame> frames)
            : session_ { std::move(session) }
            , frames_ { std::move(frames) }
        {
        }

        void next(UserlandHleCall& call)
        {
            if (frame_index_ == frames_.size()) {
                call.set_return(0U);
                return;
            }
            if (!frames_[frame_index_].display) {
                emit(call, 0U);
                return;
            }
            if (!call.memory().write32(session_->scratch, 0U)) {
                emit(call, video_decode_failed);
                return;
            }
            invoke(call, "_CVPixelBufferPoolCreatePixelBuffer",
                { 0U, session_->pool, session_->scratch }, Step::Allocate);
        }

    private:
        enum class Step {
            Allocate,
            Lock,
            PixelFormat,
            BaseAddress,
            RowBytes,
            Unlock,
            Release
        };

        void invoke(UserlandHleCall& call, std::string_view name,
            std::initializer_list<std::uint32_t> values, Step step)
        {
            arguments(call, values);
            if (!call.call_guest_function(name,
                    [self = shared_from_this(), step](UserlandHleCall& ready) {
                        self->advance(ready, step);
                    }))
                call.set_return(bad_argument);
        }

        void advance(UserlandHleCall& call, Step step)
        {
            const auto result = call.argument(0);
            switch (step) {
            case Step::Allocate:
                buffer_ = call.memory().read32(session_->scratch).value_or(0U);
                if (result || !buffer_) {
                    emit(call, video_decode_failed);
                    return;
                }
                invoke(call, "_CVPixelBufferGetPixelFormatType", { buffer_ },
                    Step::PixelFormat);
                return;
            case Step::PixelFormat:
                if (result != pixel_format_bgra) {
                    emit(call, video_decode_failed);
                    return;
                }
                invoke(call, "_CVPixelBufferLockBaseAddress", { buffer_, 0U },
                    Step::Lock);
                return;
            case Step::Lock:
                if (result) {
                    emit(call, video_decode_failed);
                    return;
                }
                invoke(call, "_CVPixelBufferGetBaseAddress", { buffer_ },
                    Step::BaseAddress);
                return;
            case Step::BaseAddress:
                base_address_ = result;
                invoke(call, "_CVPixelBufferGetBytesPerRow", { buffer_ },
                    Step::RowBytes);
                return;
            case Step::RowBytes: {
                const auto& frame = frames_[frame_index_];
                copied_ = frame.width == session_->width &&
                          frame.height == session_->height &&
                          copy_rows(call.memory(), frame.bgra, frame.width * 4U,
                              frame.height, base_address_, result);
                invoke(call, "_CVPixelBufferUnlockBaseAddress", { buffer_, 0U },
                    Step::Unlock);
                return;
            }
            case Step::Unlock:
                emit(call, result || !copied_ ? video_decode_failed : 0U);
                return;
            case Step::Release:
                ++frame_index_;
                buffer_ = 0U;
                next(call);
                return;
            }
        }

        static bool copy_rows(AddressSpace& memory,
            std::span<const std::byte> pixels, std::uint32_t width,
            std::uint32_t height, std::uint32_t address, std::uint32_t stride)
        {
            if (!address || stride < width || pixels.size() != width * height ||
                std::uint64_t { stride } * (height - 1U) + width >
                    std::numeric_limits<std::uint32_t>::max() - address)
                return false;
            if (stride == width)
                return memory.copy_to_user(address, pixels);
            for (std::uint32_t row = 0; row < height; ++row) {
                if (!memory.copy_to_user(address + row * stride,
                        pixels.subspan(row * width, width)))
                    return false;
            }
            return true;
        }

        void emit(UserlandHleCall& call, std::uint32_t status)
        {
            const auto& frame = frames_[frame_index_];
            arguments(call, { session_->callback_context, frame.identifier,
                                status == 0U ? buffer_ : 0U, status });
            if (call.call_guest_callback(session_->callback,
                    [self = shared_from_this()](UserlandHleCall& returned) {
                        self->release(returned);
                    })) {
                if (status == 0U && buffer_ && !session_->reported_output) {
                    session_->reported_output = true;
                    call.output().line(
                        "[video] first H.264 frame submitted pid=" +
                        std::to_string(call.process_id()));
                }
                return;
            }
            release(call);
        }

        void release(UserlandHleCall& call)
        {
            if (buffer_) {
                invoke(call, "_CFRelease", { buffer_ }, Step::Release);
                return;
            }
            ++frame_index_;
            next(call);
        }

        std::shared_ptr<DecoderSession> session_;
        std::vector<DecodedVideoFrame> frames_;
        std::size_t frame_index_ { };
        std::uint32_t buffer_ { };
        std::uint32_t base_address_ { };
        bool copied_ { };
    };

    class Vxd390VideoHle : public std::enable_shared_from_this<Vxd390VideoHle> {
    public:
        void install(UserlandHleRegistry& registry)
        {
            auto self = shared_from_this();
            for (const auto* name : core_video_functions)
                registry.register_guest_function(
                    "/CoreVideo.framework/CoreVideo", name);
            registry.register_guest_function(
                "/CoreFoundation.framework/CoreFoundation", "_CFRelease");
            registry.register_guest_function("/libsystem_c.dylib", "_realloc");
            registry.register_guest_function("/libsystem_c.dylib", "_free");
            registry.register_function(std::string { h264_plugin },
                pixel_attributes, [self](UserlandHleCall& call) {
                    if (self->h264_enabled_) {
                        // The plug-in passes its requested pixel format by
                        // address. Keep the rest of its IOSurface attributes.
                        const auto format = call.argument(4);
                        if (!format ||
                            !call.memory().write32(format, pixel_format_bgra)) {
                            call.set_return(bad_argument);
                            return;
                        }
                    }
                    call.resume_original_persistently();
                });
            registry.register_function(std::string { h264_plugin },
                "_H264H4Register", [self](UserlandHleCall& call) {
                    if (!h264_decoder_factory()) {
                        call.resume_original_persistently();
                        return;
                    }
                    const auto processor = call.cpu().processor_id();
                    self->registering_.insert(processor);
                    call.resume_original_persistently(
                        [self, processor](UserlandHleCall&) {
                            self->registering_.erase(processor);
                        });
                });
            registry.register_function(std::string { vxd_image },
                "_AppleVXD390CheckPlatform", [self](UserlandHleCall& call) {
                    if (!self->registering_.contains(
                            call.cpu().processor_id()) ||
                        !std::ranges::all_of(core_video_functions,
                            [&](const auto* name) {
                                return call.symbol_address(name).has_value();
                            }) ||
                        !call.symbol_address(pixel_attributes) ||
                        !call.symbol_address("_CFRelease") ||
                        !call.symbol_address("_realloc") ||
                        !call.symbol_address("_free")) {
                        call.resume_original_persistently();
                        return;
                    }
                    self->h264_enabled_ = true;
                    call.set_return(1U);
                });
            const auto add = [&](const char* name, auto method) {
                registry.register_function(std::string { vxd_image }, name,
                    [self, method](UserlandHleCall& call) {
                        const auto found =
                            self->sessions_.find(call.argument(0));
                        if (found == self->sessions_.end()) {
                            call.resume_original_persistently();
                            return;
                        }
                        ((*self).*method)(call, found->second);
                    });
            };
            registry.register_function(std::string { vxd_image },
                "_AppleVXD390OpenConnection",
                [self](UserlandHleCall& call) { self->open(call); });
            add("_AppleVXD390InitializeDecoder", &Vxd390VideoHle::initialize);
            add("_AppleVXD390GetParameter", &Vxd390VideoHle::get_parameter);
            add("_AppleVXD390SetParameter", &Vxd390VideoHle::set_parameter);
            add("_AppleVXD390DecodeFrame", &Vxd390VideoHle::decode);
            add("_AppleVXD390TerminateDecoder", &Vxd390VideoHle::terminate);
            add("_AppleVXD390CloseConnection", &Vxd390VideoHle::close);
        }

    private:
        void open(UserlandHleCall& call)
        {
            if (!h264_enabled_) {
                call.resume_original_persistently();
                return;
            }
            const auto output = call.argument(0);
            if (!output ||
                !call.memory().accessible(
                    output, sizeof(std::uint32_t), MemoryPermission::Write)) {
                call.set_return(bad_argument);
                return;
            }
            auto session = std::make_shared<DecoderSession>();
            session->scratch = call.allocate_data(
                sizeof(std::uint32_t), alignof(std::uint32_t));
            if (!session->scratch) {
                call.set_return(no_memory);
                return;
            }
            const auto handle = next_handle_++;
            if (!call.memory().write32(output, handle)) {
                call.set_return(bad_argument);
                return;
            }
            sessions_.emplace(handle, std::move(session));
            call.set_return(0U);
        }

        void initialize(
            UserlandHleCall& call, std::shared_ptr<DecoderSession> session)
        {
            const auto config = call.argument(1);
            const auto codec = call.memory().read32(config).value_or(0U);
            const auto width = call.memory().read32(config + 4U).value_or(0U);
            const auto height = call.memory().read32(config + 8U).value_or(0U);
            const auto pointer =
                call.memory().read32(config + 12U).value_or(0U);
            const auto size = call.memory().read32(config + 16U).value_or(0U);
            const auto length_size = size >> 24U;
            const auto data_size = size & 0xffffffU;
            const auto factory = h264_decoder_factory();
            if (codec != 2U || !factory || !width || !height ||
                width > maximum_dimension || height > maximum_dimension ||
                !pointer || !data_size || data_size > 64U * 1024U) {
                call.set_return(unsupported);
                return;
            }
            const auto config_bytes =
                call.memory().read_bytes(pointer, data_size);
            const auto parameter_sets =
                config_bytes ? annex_b(*config_bytes, length_size)
                             : std::nullopt;
            auto decoder = parameter_sets ? factory(*parameter_sets) : nullptr;
            if (!decoder) {
                call.set_return(bad_argument);
                return;
            }
            session->decoder = std::move(decoder);
            session->width = width;
            session->height = height;
            session->length_size = length_size;
            session->reported_output = false;
            call.output().line("[video] H.264 session pid=" +
                               std::to_string(call.process_id()) +
                               " size=" + std::to_string(width) + "x" +
                               std::to_string(height));
            call.set_return(0U);
        }

        void set_parameter(
            UserlandHleCall& call, std::shared_ptr<DecoderSession> session)
        {
            const auto value = call.argument(2);
            switch (call.argument(1)) {
            case 1U: // Display callback passed by address.
                session->callback = call.memory().read32(value).value_or(0U);
                break;
            case 2U:
                session->callback_context = value;
                break;
            case 3U:
                session->pool = value;
                break;
            case 11U: // Decoder usage hint for the hardware implementation.
                break;
            default:
                call.set_return(unsupported);
                return;
            }
            call.set_return(0U);
        }

        void get_parameter(
            UserlandHleCall& call, std::shared_ptr<DecoderSession> session)
        {
            const auto output = call.argument(2);
            switch (call.argument(1)) {
            case 7U:
                reserve_bitstream(call, std::move(session), output);
                return;
            case 8U: // Hardware platform option. Use standard CoreVideo planes.
                call.set_return(output && call.memory().write32(output, 0U)
                                    ? 0U
                                    : bad_argument);
                return;
            case 10U: {
                const std::array<std::uint32_t, 8> geometry {
                    (session->width + 15U) / 16U, (session->height + 15U) / 16U
                };
                call.set_return(
                    output && call.memory().copy_to_user(
                                  output, std::as_bytes(std::span { geometry }))
                        ? 0U
                        : bad_argument);
                return;
            }
            default:
                call.set_return(unsupported);
                return;
            }
        }

        static void reserve_bitstream(UserlandHleCall& call,
            std::shared_ptr<DecoderSession> session, std::uint32_t output)
        {
            const auto requested =
                call.memory().read32(output + 4U).value_or(0U);
            if (!output || requested > maximum_access_unit) {
                call.set_return(bad_argument);
                return;
            }
            if (session->bitstream &&
                session->bitstream_capacity >= requested) {
                call.set_return(
                    call.memory().write32(output, session->bitstream)
                        ? 0U
                        : bad_argument);
                return;
            }
            const auto capacity =
                std::max(64U * 1024U, (requested + 4095U) & ~4095U);
            arguments(call, { session->bitstream, capacity });
            if (!call.call_guest_function("_realloc",
                    [session, capacity, output](UserlandHleCall& ready) {
                        const auto pointer = ready.argument(0);
                        if (!pointer) {
                            ready.set_return(no_memory);
                            return;
                        }
                        session->bitstream = pointer;
                        session->bitstream_capacity = capacity;
                        ready.set_return(ready.memory().write32(output, pointer)
                                             ? 0U
                                             : bad_argument);
                    }))
                call.set_return(no_memory);
        }

        void decode(
            UserlandHleCall& call, std::shared_ptr<DecoderSession> session)
        {
            const auto descriptor = call.argument(1);
            const auto pointer = call.memory().read32(descriptor).value_or(0U);
            const auto length =
                call.memory().read32(descriptor + 4U).value_or(0U);
            const auto identifier =
                call.memory().read32(descriptor + 8U).value_or(0U);
            const auto display =
                (call.memory().read8(descriptor + 12U).value_or(0U) & 1U) != 0U;
            if (!descriptor || !session->decoder || !session->pool ||
                !session->callback || length > maximum_access_unit ||
                (length != 0U && !pointer)) {
                call.set_return(bad_argument);
                return;
            }
            const auto input =
                length != 0U
                    ? call.memory().read_bytes(pointer, length)
                    : std::optional<std::vector<std::byte>> { std::in_place };
            const auto packet =
                input ? annex_b(*input, session->length_size) : std::nullopt;
            auto frames =
                packet ? session->decoder->decode(*packet, identifier, display)
                       : std::nullopt;
            if (!frames || !call.memory().write32(descriptor + 20U, 0U)) {
                call.set_return(bad_argument);
                return;
            }
            std::make_shared<FrameDelivery>(
                std::move(session), std::move(*frames))
                ->next(call);
        }

        void terminate(
            UserlandHleCall& call, std::shared_ptr<DecoderSession> session)
        {
            session->decoder.reset();
            call.set_return(0U);
        }

        void close(
            UserlandHleCall& call, std::shared_ptr<DecoderSession> session)
        {
            sessions_.erase(call.argument(0));
            if (session->bitstream) {
                arguments(call, { session->bitstream });
                if (call.call_guest_function(
                        "_free", [](UserlandHleCall& returned) {
                            returned.set_return(0U);
                        }))
                    return;
            }
            call.set_return(0U);
        }

        std::map<std::uint32_t, std::shared_ptr<DecoderSession>> sessions_;
        std::set<std::size_t> registering_;
        std::uint32_t next_handle_ { 0x70000000U };
        bool h264_enabled_ { };
    };
} // namespace

void register_vxd390_video_hle(UserlandHleRegistry& registry)
{
    std::make_shared<Vxd390VideoHle>()->install(registry);
}
} // namespace ilemu
