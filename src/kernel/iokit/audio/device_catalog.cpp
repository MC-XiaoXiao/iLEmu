// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// Build virtual audio device identities and registry properties.

#include "kernel/kernel_iokit_audio_device_catalog.hpp"

#include <algorithm>
#include <array>

namespace ilemu::kernel_iokit::audio {
namespace {

    constexpr std::uint32_t four_cc(
        char first, char second, char third, char fourth)
    {
        return static_cast<std::uint32_t>(static_cast<unsigned char>(first))
                   << 24U |
               static_cast<std::uint32_t>(static_cast<unsigned char>(second))
                   << 16U |
               static_cast<std::uint32_t>(static_cast<unsigned char>(third))
                   << 8U |
               static_cast<std::uint32_t>(static_cast<unsigned char>(fourth));
    }

    constexpr std::uint32_t linear_pcm_format = four_cc('l', 'p', 'c', 'm');
    // kAudioDeviceTransportTypeBuiltIn.  The value is a four-character code
    // represented in the same big-endian form used by CoreAudio properties.
    constexpr std::uint32_t built_in_transport_type =
        four_cc('b', 'l', 't', 'n');
    constexpr std::uint32_t linear_pcm_flags = 0x0cU;
    constexpr std::uint32_t selector_control_base_class =
        four_cc('s', 'l', 'c', 't');
    constexpr std::uint32_t boolean_control_base_class =
        four_cc('t', 'o', 'g', 'l');
    constexpr std::uint32_t level_control_base_class =
        four_cc('l', 'e', 'v', 'l');
    constexpr std::uint32_t data_source_control_class =
        four_cc('d', 's', 'r', 'c');
    constexpr std::uint32_t jack_control_class = four_cc('j', 'a', 'c', 'k');
    constexpr std::uint32_t playthrough_route_control_class =
        four_cc('f', 'm', '2', 'r');
    constexpr std::uint32_t baseband_to_codec_route_control_class =
        four_cc('b', 'b', '2', 'w');
    constexpr std::uint32_t codec_to_baseband_route_control_class =
        four_cc('w', '2', 'b', 'b');
    constexpr std::uint32_t device_mute_control_class =
        four_cc('d', 'm', 'u', 't');
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

    constexpr std::array codec_input_sources { IOAudio2SelectorItemDescription {
                                                   internal_microphone_source,
                                                   "Internal Microphone" },
        IOAudio2SelectorItemDescription {
            external_microphone_source, "External Microphone" },
        IOAudio2SelectorItemDescription { line_input_source, "Line in" } };

    constexpr IOAudio2ControlRangeDescription master_output_range {
        0xffffff8100000000ULL, 254U, 0U, 0x0000000080000000ULL
    };
    constexpr IOAudio2ControlRangeDescription channel_output_range {
        0xffffffc700000000ULL, 63U, 0U, 0x0000000100000000ULL
    };
    constexpr IOAudio2ControlRangeDescription input_gain_range {
        0xfffffff340000000ULL, 64U, 0U, 0x00000000c0000000ULL
    };

    // Output elements identify codec endpoints, not PCM stream channels:
    // headphones, line out, receiver and speaker.  The native HAL discovers
    // each endpoint through its volume/mute controls, including the built-in
    // outputs that remain available when no external jack is connected.
    constexpr std::array<IOAudio2ControlDescription, 17> codec_controls { {
        { 3U, boolean_control_base_class, jack_control_class, output_scope, 0U,
            0U, true, std::nullopt, { } },
        { 16U, boolean_control_base_class, playthrough_route_control_class,
            playthrough_scope, 0U, 0U, false, std::nullopt, { } },
        { 17U, boolean_control_base_class,
            baseband_to_codec_route_control_class, playthrough_scope, 0U, 0U,
            false, std::nullopt, { } },
        { 18U, boolean_control_base_class,
            codec_to_baseband_route_control_class, playthrough_scope, 0U, 0U,
            false, std::nullopt, { } },
        { 6U, boolean_control_base_class, device_mute_control_class,
            output_scope, 0U, 0U, false, std::nullopt, { } },
        { 5U, boolean_control_base_class, mute_control_class, output_scope, 0U,
            0U, false, std::nullopt, { } },
        { 7U, level_control_base_class, volume_control_class, output_scope, 0U,
            254U, false, master_output_range, { } },
        { 8U, level_control_base_class, volume_control_class, output_scope, 1U,
            63U, false, channel_output_range, { } },
        { 9U, boolean_control_base_class, mute_control_class, output_scope, 1U,
            0U, false, std::nullopt, { } },
        { 10U, level_control_base_class, volume_control_class, output_scope, 2U,
            63U, false, channel_output_range, { } },
        { 11U, boolean_control_base_class, mute_control_class, output_scope, 2U,
            0U, false, std::nullopt, { } },
        { 12U, level_control_base_class, volume_control_class, output_scope, 3U,
            63U, false, channel_output_range, { } },
        { 13U, boolean_control_base_class, mute_control_class, output_scope, 3U,
            0U, false, std::nullopt, { } },
        { 14U, level_control_base_class, volume_control_class, output_scope, 4U,
            63U, false, channel_output_range, { } },
        { 15U, boolean_control_base_class, mute_control_class, output_scope, 4U,
            0U, false, std::nullopt, { } },
        { 19U, level_control_base_class, volume_control_class, input_scope, 0U,
            17U, false, input_gain_range, { } },
        { 20U, selector_control_base_class, data_source_control_class,
            input_scope, 0U, internal_microphone_source, false, std::nullopt,
            codec_input_sources },
    } };

    constexpr IOAudio2StreamFormatDescription mono_pcm_44100 {
        .sample_rate = 44100,
        .format_id = linear_pcm_format,
        .format_flags = linear_pcm_flags,
        .bytes_per_packet = 2,
        .frames_per_packet = 1,
        .bytes_per_frame = 2,
        .channels_per_frame = 1,
        .bits_per_channel = 16,
    };

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
        .format = mono_pcm_44100,
        .available_formats = {},
    },
};

    constexpr IOAudio2StreamFormatDescription baseband_media_format {
        .sample_rate = 44100,
        .format_id = linear_pcm_format,
        .format_flags = linear_pcm_flags,
        .bytes_per_packet = 4,
        .frames_per_packet = 1,
        .bytes_per_frame = 4,
        .channels_per_frame = 2,
        .bits_per_channel = 16,
    };

    constexpr IOAudio2StreamFormatDescription baseband_telephony_format {
        .sample_rate = 8000,
        .format_id = linear_pcm_format,
        .format_flags = linear_pcm_flags,
        .bytes_per_packet = 2,
        .frames_per_packet = 1,
        .bytes_per_frame = 2,
        .channels_per_frame = 1,
        .bits_per_channel = 16,
    };

    constexpr std::array baseband_formats { baseband_media_format,
        baseband_telephony_format };

    // The modem path is a distinct IOAudio2 endpoint. VirtualAudio turns this
    // endpoint into firmware-owned uplink/downlink and receiver/speaker ports.
    // It begins in the media-rate format and can negotiate the narrow-band call
    // format through the ordinary IOAudio2 stream-format selector.
    constexpr std::array baseband_streams {
        IOAudio2StreamDescription {
            .identifier = 1,
            .direction = IOAudio2StreamDirection::Output,
            .starting_channel = 1,
            .buffer_mapping_options = 1,
            .buffer_size = 4096,
            .format = baseband_media_format,
            .available_formats = baseband_formats,
        },
        IOAudio2StreamDescription {
            .identifier = 2,
            .direction = IOAudio2StreamDirection::Input,
            .starting_channel = 1,
            .buffer_mapping_options = 1,
            .buffer_size = 4096,
            .format = baseband_media_format,
            .available_formats = baseband_formats,
        },
    };

    // The native routing engine selects the source of the Voice endpoint.
    constexpr auto application_voice_source = four_cc('a', 'p', '2', 'v');
    constexpr std::array voice_sources {
        IOAudio2SelectorItemDescription {
            application_voice_source, "Application Processor" },
    };
    constexpr std::array voice_controls {
        IOAudio2ControlDescription { 1U, selector_control_base_class,
            data_source_control_class, four_cc('g', 'l', 'o', 'b'), 0U,
            application_voice_source, false, std::nullopt, voice_sources },
    };

    // The amplifier plays one mono channel and samples four feedback
    // channels. VirtualAudio enumerates the feedback sources to build its
    // speaker-protection monitoring port.
    constexpr std::uint32_t speaker_period_frames = 16384;
    constexpr IOAudio2StreamFormatDescription speaker_monitor_format {
        .sample_rate = 44100,
        .format_id = linear_pcm_format,
        .format_flags = linear_pcm_flags,
        .bytes_per_packet = 8,
        .frames_per_packet = 1,
        .bytes_per_frame = 8,
        .channels_per_frame = 4,
        .bits_per_channel = 16,
    };
    constexpr std::array speaker_protection_streams {
        IOAudio2StreamDescription {
            .identifier = 100,
            .direction = IOAudio2StreamDirection::Input,
            .starting_channel = 1,
            .buffer_mapping_options = 1,
            .buffer_size =
                speaker_period_frames * speaker_monitor_format.bytes_per_frame,
            .format = speaker_monitor_format,
            .available_formats = { },
        },
        IOAudio2StreamDescription {
            .identifier = 200,
            .direction = IOAudio2StreamDirection::Output,
            .starting_channel = 1,
            .buffer_mapping_options = 1,
            .buffer_size =
                speaker_period_frames * mono_pcm_44100.bytes_per_frame,
            .format = mono_pcm_44100,
            .available_formats = { },
        },
    };
    constexpr auto speaker_voltage = four_cc('s', 'p', 'V', '0');
    constexpr auto speaker_current = four_cc('s', 'p', 'I', '0');
    constexpr auto speaker_supply_voltage = four_cc('s', 'p', 'P', '0');
    constexpr auto speaker_boost_voltage = four_cc('s', 'p', 'B', '0');
    constexpr std::array speaker_monitor_sources {
        IOAudio2SelectorItemDescription { 0, "Disabled" },
        IOAudio2SelectorItemDescription {
            speaker_voltage, "Speaker 0 voltage" },
        IOAudio2SelectorItemDescription {
            speaker_current, "Speaker 0 current" },
        IOAudio2SelectorItemDescription {
            speaker_supply_voltage, "Speaker 0 power supply voltage" },
        IOAudio2SelectorItemDescription {
            speaker_boost_voltage, "Speaker 0 boost voltage" },
        IOAudio2SelectorItemDescription {
            four_cc('s', 'p', 'F', '0'), "Speaker 0 flags" },
    };
    constexpr std::array speaker_monitor_channels {
        speaker_voltage,
        speaker_current,
        speaker_supply_voltage,
        speaker_boost_voltage,
    };
    constexpr IOAudio2ControlRangeDescription amplifier_volume_range {
        0xffffff9a00000000ULL, 228U, 0U, 0x0000000080000000ULL
    };
    constexpr std::array speaker_protection_controls {
        IOAudio2ControlDescription { 304U, boolean_control_base_class,
            device_mute_control_class, output_scope, 0U, 0U, false,
            std::nullopt, { } },
        IOAudio2ControlDescription { 303U, boolean_control_base_class,
            mute_control_class, output_scope, 0U, 0U, false, std::nullopt,
            { } },
        IOAudio2ControlDescription { 305U, level_control_base_class,
            volume_control_class, output_scope, 0U, 204U, false,
            amplifier_volume_range, { } },
        IOAudio2ControlDescription { 350U, selector_control_base_class,
            data_source_control_class, input_scope, 0U, 0U, false, std::nullopt,
            speaker_monitor_sources,
            std::span<const std::uint32_t> { speaker_monitor_channels } },
    };

    constexpr std::array device_catalog {
        IOAudio2DeviceDescription {
            .name = "Built-in Audio",
            .manufacturer = "Apple Computer, Inc.",
            .uid = "Codec",
            .transport_type = built_in_transport_type,
            .io_buffer_frame_size = 1024,
            .streams = codec_streams,
            .controls = codec_controls,
        },
        IOAudio2DeviceDescription {
            .name = "Baseband",
            .manufacturer = "Apple Computer, Inc.",
            .uid = "Baseband",
            .transport_type = built_in_transport_type,
            .io_buffer_frame_size = 1024,
            .streams = baseband_streams,
            .controls = { },
        },
        IOAudio2DeviceDescription {
            .name = "Voice",
            .manufacturer = "Apple Computer, Inc.",
            .uid = "Voice",
            .transport_type = built_in_transport_type,
            .io_buffer_frame_size = 1024,
            .streams = { },
            .controls = voice_controls,
        },
    };

    // The separate-amplifier topology has a duplex Voice port. VirtualAudio
    // enumerates both stream IDs before it can construct the routing graph.
    constexpr std::uint32_t voice_period_frames = 3072;
    constexpr auto voice_formats = [] {
        constexpr std::array rates { 8000U, 11025U, 12000U, 16000U, 22050U,
            24000U, 32000U, 44100U, 48000U };
        std::array<IOAudio2StreamFormatDescription, rates.size()> formats;
        for (std::size_t index = 0; index < rates.size(); ++index) {
            formats[index] = baseband_telephony_format;
            formats[index].sample_rate = rates[index];
        }
        return formats;
    }();
    constexpr auto voice_stream = [](std::uint32_t id,
                                      IOAudio2StreamDirection direction) {
        return IOAudio2StreamDescription {
            .identifier = id,
            .direction = direction,
            .starting_channel = 1,
            .buffer_mapping_options = 1,
            .buffer_size =
                voice_period_frames * baseband_telephony_format.bytes_per_frame,
            .format = baseband_telephony_format,
            .available_formats = voice_formats,
        };
    };
    constexpr std::array duplex_voice_streams {
        voice_stream(100, IOAudio2StreamDirection::Input),
        voice_stream(200, IOAudio2StreamDirection::Output),
    };
    constexpr auto duplex_voice = [] {
        auto device = device_catalog[2];
        device.io_buffer_frame_size = voice_period_frames;
        device.streams = duplex_voice_streams;
        return device;
    }();

    constexpr std::array speaker_protection_device_catalog {
        device_catalog[0],
        device_catalog[1],
        duplex_voice,
        IOAudio2DeviceDescription {
            .name = "CS35L19",
            .manufacturer = "Apple Computer, Inc.",
            .uid = "Speaker",
            .transport_type = built_in_transport_type,
            .io_buffer_frame_size = speaker_period_frames,
            .streams = speaker_protection_streams,
            .controls = speaker_protection_controls,
        },
    };

} // namespace

std::span<const IOAudio2DeviceDescription> IOAudio2DeviceCatalog::devices(
    AudioHardwareProfile profile)
{
    if (profile ==
        AudioHardwareProfile::CodecBasebandVoiceRoutingSpeakerProtection)
        return std::span { speaker_protection_device_catalog };

    const auto devices = std::span { device_catalog };
    return profile == AudioHardwareProfile::CodecBasebandVoiceRouting
               ? devices
               : devices.first(2U);
}

const IOAudio2DeviceDescription* IOAudio2DeviceCatalog::find(
    std::string_view uid, AudioHardwareProfile profile)
{
    const auto available_devices = devices(profile);
    const auto device =
        std::find_if(available_devices.begin(), available_devices.end(),
            [uid](const auto& candidate) { return candidate.uid == uid; });
    return device == available_devices.end() ? nullptr : &*device;
}

} // namespace ilemu::kernel_iokit::audio
