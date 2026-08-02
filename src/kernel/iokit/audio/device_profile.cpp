#include "ilemu/kernel_iokit_audio_device_profile.hpp"

#include <algorithm>
#include <array>

namespace ilemu::kernel_iokit::audio {
namespace {

constexpr std::uint32_t four_cc(char first, char second, char third,
                                char fourth) {
  return static_cast<std::uint32_t>(static_cast<unsigned char>(first)) << 24U |
         static_cast<std::uint32_t>(static_cast<unsigned char>(second)) << 16U |
         static_cast<std::uint32_t>(static_cast<unsigned char>(third)) << 8U |
         static_cast<std::uint32_t>(static_cast<unsigned char>(fourth));
}

constexpr std::uint32_t linear_pcm_format = four_cc('l', 'p', 'c', 'm');
constexpr std::uint32_t linear_pcm_flags = 0x0cU;
constexpr std::uint32_t selector_control_base_class =
    four_cc('s', 'l', 'c', 't');
constexpr std::uint32_t boolean_control_base_class =
    four_cc('t', 'o', 'g', 'l');
constexpr std::uint32_t level_control_base_class = four_cc('l', 'e', 'v', 'l');
constexpr std::uint32_t data_source_control_class = four_cc('d', 's', 'r', 'c');
constexpr std::uint32_t jack_control_class = four_cc('j', 'a', 'c', 'k');
constexpr std::uint32_t playthrough_route_control_class =
    four_cc('f', 'm', '2', 'r');
constexpr std::uint32_t baseband_to_codec_route_control_class =
    four_cc('b', 'b', '2', 'w');
constexpr std::uint32_t codec_to_baseband_route_control_class =
    four_cc('w', '2', 'b', 'b');
constexpr std::uint32_t device_mute_control_class = four_cc('d', 'm', 'u', 't');
constexpr std::uint32_t mute_control_class = four_cc('m', 'u', 't', 'e');
constexpr std::uint32_t volume_control_class = four_cc('v', 'l', 'm', 'e');
constexpr std::uint32_t output_scope = four_cc('o', 'u', 't', 'p');
constexpr std::uint32_t input_scope = four_cc('i', 'n', 'p', 't');
constexpr std::uint32_t playthrough_scope = four_cc('p', 't', 'r', 'u');
constexpr std::uint32_t internal_microphone_source =
    four_cc('i', 'm', 'i', 'c');
constexpr std::uint32_t external_microphone_source =
    four_cc('e', 'm', 'i', 'c');
constexpr std::uint32_t line_input_source = four_cc('l', 'i', 'n', 'e');

constexpr std::array codec_input_sources{
    IOAudio2SelectorItemDescription{internal_microphone_source,
                                    "Internal Microphone"},
    IOAudio2SelectorItemDescription{external_microphone_source,
                                    "External Microphone"},
    IOAudio2SelectorItemDescription{line_input_source, "Line in"}};

constexpr IOAudio2ControlRangeDescription master_output_range{
    0xffffff8100000000ULL, 254U, 0U, 0x0000000080000000ULL};
constexpr IOAudio2ControlRangeDescription channel_output_range{
    0xffffffc700000000ULL, 63U, 0U, 0x0000000100000000ULL};
constexpr IOAudio2ControlRangeDescription input_gain_range{
    0xfffffff340000000ULL, 64U, 0U, 0x00000000c0000000ULL};

constexpr std::array<IOAudio2ControlDescription, 13> codec_controls{{
    {3U,
     boolean_control_base_class,
     jack_control_class,
     output_scope,
     0U,
     0U,
     true,
     std::nullopt,
     {}},
    {16U,
     boolean_control_base_class,
     playthrough_route_control_class,
     playthrough_scope,
     0U,
     0U,
     false,
     std::nullopt,
     {}},
    {17U,
     boolean_control_base_class,
     baseband_to_codec_route_control_class,
     playthrough_scope,
     0U,
     0U,
     false,
     std::nullopt,
     {}},
    {18U,
     boolean_control_base_class,
     codec_to_baseband_route_control_class,
     playthrough_scope,
     0U,
     0U,
     false,
     std::nullopt,
     {}},
    {6U,
     boolean_control_base_class,
     device_mute_control_class,
     output_scope,
     0U,
     0U,
     false,
     std::nullopt,
     {}},
    {5U,
     boolean_control_base_class,
     mute_control_class,
     output_scope,
     0U,
     0U,
     false,
     std::nullopt,
     {}},
    {7U,
     level_control_base_class,
     volume_control_class,
     output_scope,
     0U,
     254U,
     false,
     master_output_range,
     {}},
    {8U,
     level_control_base_class,
     volume_control_class,
     output_scope,
     1U,
     63U,
     false,
     channel_output_range,
     {}},
    {9U,
     boolean_control_base_class,
     mute_control_class,
     output_scope,
     1U,
     0U,
     false,
     std::nullopt,
     {}},
    {10U,
     level_control_base_class,
     volume_control_class,
     output_scope,
     2U,
     63U,
     false,
     channel_output_range,
     {}},
    {11U,
     boolean_control_base_class,
     mute_control_class,
     output_scope,
     2U,
     0U,
     false,
     std::nullopt,
     {}},
    {19U,
     level_control_base_class,
     volume_control_class,
     input_scope,
     0U,
     17U,
     false,
     input_gain_range,
     {}},
    {20U, selector_control_base_class, data_source_control_class, input_scope,
     0U, internal_microphone_source, false, std::nullopt, codec_input_sources},
}};

constexpr std::array codec_streams{
    IOAudio2StreamDescription{
        .identifier = 1,
        .direction = IOAudio2StreamDirection::Output,
        .starting_channel = 1,
        .buffer_mapping_options = 1,
        .buffer_size = 4096,
        .format =
            {
                .sample_rate = 44100,
                .format_id = linear_pcm_format,
                .format_flags = linear_pcm_flags,
                .bytes_per_packet = 4,
                .frames_per_packet = 1,
                .bytes_per_frame = 4,
                .channels_per_frame = 2,
                .bits_per_channel = 16,
            },
        .available_formats = {},
    },
    IOAudio2StreamDescription{
        .identifier = 2,
        .direction = IOAudio2StreamDirection::Input,
        .starting_channel = 1,
        .buffer_mapping_options = 1,
        .buffer_size = 2048,
        .format =
            {
                .sample_rate = 44100,
                .format_id = linear_pcm_format,
                .format_flags = linear_pcm_flags,
                .bytes_per_packet = 2,
                .frames_per_packet = 1,
                .bytes_per_frame = 2,
                .channels_per_frame = 1,
                .bits_per_channel = 16,
            },
        .available_formats = {},
    },
};

constexpr IOAudio2StreamFormatDescription baseband_media_format{
    .sample_rate = 44100,
    .format_id = linear_pcm_format,
    .format_flags = linear_pcm_flags,
    .bytes_per_packet = 4,
    .frames_per_packet = 1,
    .bytes_per_frame = 4,
    .channels_per_frame = 2,
    .bits_per_channel = 16,
};

constexpr IOAudio2StreamFormatDescription baseband_telephony_format{
    .sample_rate = 8000,
    .format_id = linear_pcm_format,
    .format_flags = linear_pcm_flags,
    .bytes_per_packet = 2,
    .frames_per_packet = 1,
    .bytes_per_frame = 2,
    .channels_per_frame = 1,
    .bits_per_channel = 16,
};

constexpr std::array baseband_formats{baseband_media_format,
                                      baseband_telephony_format};

// The modem path is a distinct IOAudio2 endpoint. VirtualAudio turns this
// endpoint into firmware-owned uplink/downlink and receiver/speaker ports.
// It begins in the media-rate format and can negotiate the narrow-band call
// format through the ordinary IOAudio2 stream-format selector.
constexpr std::array baseband_streams{
    IOAudio2StreamDescription{
        .identifier = 1,
        .direction = IOAudio2StreamDirection::Output,
        .starting_channel = 1,
        .buffer_mapping_options = 1,
        .buffer_size = 4096,
        .format = baseband_media_format,
        .available_formats = baseband_formats,
    },
    IOAudio2StreamDescription{
        .identifier = 2,
        .direction = IOAudio2StreamDirection::Input,
        .starting_channel = 1,
        .buffer_mapping_options = 1,
        .buffer_size = 4096,
        .format = baseband_media_format,
        .available_formats = baseband_formats,
    },
};

constexpr std::array device_catalog{
    IOAudio2DeviceDescription{
        .name = "Built-in Audio",
        .manufacturer = "Apple Computer, Inc.",
        .uid = "Codec",
        .io_buffer_frame_size = 1024,
        .streams = codec_streams,
        .controls = codec_controls,
    },
    IOAudio2DeviceDescription{
        .name = "Baseband",
        .manufacturer = "Apple Computer, Inc.",
        .uid = "Baseband",
        .io_buffer_frame_size = 1024,
        .streams = baseband_streams,
        .controls = {},
    },
};

} // namespace

std::span<const IOAudio2DeviceDescription> IOAudio2DeviceCatalog::devices() {
  return device_catalog;
}

const IOAudio2DeviceDescription *
IOAudio2DeviceCatalog::find(std::string_view uid) {
  const auto device = std::find_if(
      device_catalog.begin(), device_catalog.end(),
      [uid](const auto &candidate) { return candidate.uid == uid; });
  return device == device_catalog.end() ? nullptr : &*device;
}

} // namespace ilemu::kernel_iokit::audio
