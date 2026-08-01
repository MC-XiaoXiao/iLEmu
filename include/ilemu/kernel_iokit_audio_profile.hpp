#pragma once

#include <cstdint>
#include <optional>
#include <string_view>

namespace ilemu::kernel_iokit::audio {

// Describes the firmware-facing IOAudio user-client ABI. Selection follows the
// provider protocol exposed in the IOKit registry, rather than an OS version,
// product build, process, or application.
struct IOKitAudioAbiProfile {
  struct RegistryKeys {
    std::string_view device_name;
    std::string_view device_manufacturer;
    std::string_view device_uid;
    std::string_view exclusive_access_owner;
    std::string_view io_buffer_frame_size;
    std::string_view input_safety_offset;
    std::string_view output_safety_offset;
    std::string_view input_latency;
    std::string_view output_latency;
    std::string_view sample_rate;
    std::string_view is_running;
    std::string_view input_streams;
    std::string_view output_streams;
    std::string_view controls;
    std::string_view stream_id;
    std::string_view starting_channel;
    std::string_view current_format;
    std::string_view available_formats;
    std::string_view buffer_mapping_options;
    std::string_view format_id;
    std::string_view format_flags;
    std::string_view bytes_per_packet;
    std::string_view frames_per_packet;
    std::string_view bytes_per_frame;
    std::string_view channels_per_frame;
    std::string_view bits_per_channel;
    std::string_view minimum_sample_rate;
    std::string_view maximum_sample_rate;
  };

  struct Selectors {
    std::uint32_t start;
    std::uint32_t stop;
    std::uint32_t set_nominal_sample_rate;
    std::uint32_t set_stream_current_format;
    std::uint32_t set_stream_active;
  };

  struct MemoryTypes {
    std::uint32_t engine_status;
    std::uint32_t engine_status_size;
    std::uint32_t stream_base;
    std::uint32_t map_options;
  };

  std::string_view service_class;
  std::string_view registry_path;
  std::uint32_t service_type;
  std::uint32_t notification_type;
  RegistryKeys registry;
  Selectors selectors;
  MemoryTypes memory;

  [[nodiscard]] std::uint32_t stream_memory_type(std::uint32_t stream_id) const;
  [[nodiscard]] std::optional<std::uint32_t>
  stream_id_for_memory_type(std::uint32_t memory_type) const;

  [[nodiscard]] static const IOKitAudioAbiProfile &io_audio2();
};

} // namespace ilemu::kernel_iokit::audio
