#include "ilemu/kernel_iokit.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <mutex>
#include <optional>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "ilemu/address_space.hpp"
#include "ilemu/device_mig_ids.hpp"
#include "ilemu/iokit_abi.hpp"
#include "ilemu/kernel_iokit_audio.hpp"
#include "ilemu/kernel_iokit_audio_profile.hpp"
#include "ilemu/kernel_iokit_baseband.hpp"
#include "ilemu/kernel_iokit_camera.hpp"
#include "ilemu/kernel_iokit_display.hpp"
#include "ilemu/kernel_iokit_graphics.hpp"
#include "ilemu/kernel_iokit_jpeg.hpp"
#include "ilemu/kernel_iokit_mbx.hpp"
#include "ilemu/kernel_iokit_mobile_file_integrity.hpp"
#include "ilemu/kernel_shared_state.hpp"
#include "ilemu/mig_wire_abi.hpp"
#include "ilemu/output.hpp"
#include "ilemu/wifi_state.hpp"

#include "iokit/power.hpp"
#include "mach/support.hpp"

namespace ilemu {
namespace {

constexpr std::uint32_t mach_rcv_invalid_data = 0x10004008U;
constexpr std::uint32_t mach_reply_bits = 0x00000012U;
constexpr std::uint32_t mach_ndr_native = 0x00000000U;
constexpr std::uint32_t mach_ndr_little_endian = 0x00000001U;
constexpr std::uint32_t mig_reply_id_delta = 100;
constexpr std::string_view apple_h1clcd_class{"AppleH1CLCD"};
constexpr std::string_view mobile_framebuffer_class{"IOMobileFramebuffer"};
constexpr std::string_view core_surface_root_class{"IOCoreSurfaceRoot"};
constexpr std::string_view io80211_controller_class{"IO80211Controller"};
constexpr std::string_view io80211_interface_class{"IO80211Interface"};
constexpr std::string_view io_ethernet_interface_class{"IOEthernetInterface"};
constexpr std::string_view io_network_interface_class{"IONetworkInterface"};
constexpr std::string_view io_network_stack_class{"IONetworkStack"};
constexpr std::string_view io_ipod_usb_device_class{"IOIpodUSBDevice"};
constexpr std::string_view wifi_bus_class{"AppleARMIODevice"};
constexpr std::string_view bsd_name_property{"BSD Name"};
constexpr std::string_view interface_name_prefix_property{
    "IOInterfaceNamePrefix"};
constexpr std::string_view interface_unit_property{"IOInterfaceUnit"};
constexpr std::string_view interface_type_property{"IOInterfaceType"};
constexpr std::string_view network_root_type_property{"IONetworkRootType"};
constexpr std::string_view builtin_interface_property{"IOBuiltin"};
constexpr std::string_view primary_interface_property{"IOPrimaryInterface"};
constexpr std::string_view mac_address_property{"IOMACAddress"};
constexpr std::string_view local_mac_address_property{"local-mac-address"};
constexpr std::string_view sdio_device_name{"sdio"};
constexpr std::size_t maximum_matching_trace_bytes = 256;
constexpr std::uint32_t io_service_matching_type = 100;
constexpr std::uint32_t io_bsd_name_matching_type = 101;
constexpr std::uint32_t io_of_path_matching_type = 102;

namespace device_mig = xnu792::mig::device;

constexpr const auto &matching_services_matching =
    device_mig::io_service_get_matching_services_arguments[1];
constexpr const auto &make_matching_arguments =
    device_mig::io_make_matching_arguments;
constexpr const auto &registry_entry_from_path =
    device_mig::io_registry_entry_from_path_arguments[1];
constexpr const auto &registry_entry_path =
    device_mig::io_registry_entry_get_path_arguments[2];
constexpr const auto &object_class_name =
    device_mig::io_object_get_class_arguments[1];
constexpr const auto &registry_entry_name =
    device_mig::io_registry_entry_get_name_arguments[1];
constexpr const auto &registry_property_name =
    device_mig::io_registry_entry_get_property_arguments[1];
constexpr const auto &registry_property_value =
    device_mig::io_registry_entry_get_property_arguments[2];
constexpr const auto &recursive_registry_property =
    device_mig::io_registry_entry_get_property_recursively_arguments;
constexpr const auto &notification_type =
    device_mig::io_service_add_notification_arguments[1];
constexpr const auto &notification_matching =
    device_mig::io_service_add_notification_arguments[2];
constexpr const auto &notification_wake_port =
    device_mig::io_service_add_notification_arguments[3];
constexpr const auto &notification_reference =
    device_mig::io_service_add_notification_arguments[4];
constexpr const auto &set_properties_data =
    device_mig::io_registry_entry_set_properties_arguments[1];
constexpr const auto &service_open_owning_task =
    device_mig::io_service_open_arguments[1];
constexpr const auto &service_open_connect_type =
    device_mig::io_service_open_arguments[2];

constexpr std::string_view platform_expert_class{"IOPlatformExpertDevice"};
constexpr std::string_view model_number_property{"model-number"};
constexpr std::string_view platform_serial_number_property{
    "IOPlatformSerialNumber"};
constexpr std::string_view compatible_property{"compatible"};
constexpr std::string_view apple_arm_compatible{"AppleARM"};

static_assert(
    device_mig::id(device_mig::Routine::io_service_get_matching_services) ==
    static_cast<std::uint32_t>(iokit_abi::Message::ServiceGetMatchingServices));
static_assert(
    device_mig::id(device_mig::Routine::io_service_add_notification) ==
    static_cast<std::uint32_t>(iokit_abi::Message::ServiceAddNotification));
static_assert(
    device_mig::id(device_mig::Routine::io_registry_entry_set_properties) ==
    static_cast<std::uint32_t>(iokit_abi::Message::RegistryEntrySetProperties));
static_assert(
    device_mig::id(device_mig::Routine::io_registry_get_root_entry) ==
    static_cast<std::uint32_t>(iokit_abi::Message::RegistryGetRootEntry));
static_assert(
    device_mig::id(device_mig::Routine::io_service_get_busy_state) ==
    static_cast<std::uint32_t>(iokit_abi::Message::ServiceGetBusyState));
static_assert(device_mig::id(device_mig::Routine::io_service_open) ==
              static_cast<std::uint32_t>(iokit_abi::Message::ServiceOpen));

struct ConnectMethodRequest {
  std::uint32_t selector{};
  std::array<std::uint64_t, iokit_abi::connect_method::maximum_scalar_count>
      scalar_input{};
  std::uint32_t scalar_input_count{};
  std::vector<std::byte> inband_input;
  std::uint32_t scalar_output_capacity{};
  std::uint32_t inband_output_capacity{};
};

struct ConnectMethodResult {
  std::uint32_t return_code{iokit_abi::unsupported};
  std::vector<std::uint64_t> scalar_output;
  std::vector<std::byte> inband_output;
};

template <std::size_t Size>
std::uint32_t write_reply(AddressSpace &memory, std::uint32_t address,
                          const std::array<std::uint32_t, Size> &reply);

constexpr std::uint32_t align_mig_field(std::uint32_t size) {
  constexpr std::uint32_t alignment = sizeof(std::uint32_t);
  return (size + alignment - 1U) & ~(alignment - 1U);
}

std::optional<ConnectMethodRequest>
read_connect_method_request(AddressSpace &memory, std::uint32_t address,
                            std::uint32_t send_size,
                            std::uint32_t receive_size) {
  using namespace iokit_abi::connect_method;
  if (send_size < minimum_request_size || receive_size < minimum_reply_size) {
    return std::nullopt;
  }

  ConnectMethodRequest request;
  request.selector = memory.read32(address + selector_offset).value_or(0);
  request.scalar_input_count =
      memory.read32(address + scalar_input_count_offset)
          .value_or(maximum_scalar_count + 1U);
  if (request.scalar_input_count > maximum_scalar_count) {
    return std::nullopt;
  }
  const auto scalar_bytes = request.scalar_input_count * scalar_size;
  const auto inband_count_offset = scalar_input_offset + scalar_bytes;
  if (inband_count_offset + inband_count_size > send_size) {
    return std::nullopt;
  }
  const auto inband_count = memory.read32(address + inband_count_offset);
  if (!inband_count || *inband_count > maximum_inband_size) {
    return std::nullopt;
  }
  const auto trailing_offset =
      inband_count_offset + inband_count_size + align_mig_field(*inband_count);
  const auto required_size = trailing_offset + trailing_request_size;
  if (send_size < required_size)
    return std::nullopt;

  for (std::uint32_t index = 0; index < request.scalar_input_count; ++index) {
    const auto scalar_address =
        address + scalar_input_offset + index * scalar_size;
    const auto low = memory.read32(scalar_address);
    const auto high = memory.read32(scalar_address + sizeof(std::uint32_t));
    if (!low || !high)
      return std::nullopt;
    request.scalar_input[index] = static_cast<std::uint64_t>(*low) |
                                  (static_cast<std::uint64_t>(*high) << 32U);
  }
  const auto inband_address = address + inband_count_offset + inband_count_size;
  if (*inband_count != 0) {
    const auto bytes = memory.read_bytes(inband_address, *inband_count);
    if (!bytes)
      return std::nullopt;
    request.inband_input = std::move(*bytes);
  }
  // ool_input occupies the first two trailing words. The two CountInOut
  // capacities immediately follow it in the firmware-generated request.
  request.scalar_output_capacity =
      memory.read32(address + trailing_offset + 2U * sizeof(std::uint32_t))
          .value_or(0);
  request.inband_output_capacity =
      memory.read32(address + trailing_offset + 3U * sizeof(std::uint32_t))
          .value_or(0);
  if (request.scalar_output_capacity > maximum_scalar_count ||
      request.inband_output_capacity > maximum_inband_size) {
    return std::nullopt;
  }
  return request;
}

std::uint32_t write_connect_method_reply(AddressSpace &memory,
                                         std::uint32_t address,
                                         std::uint32_t local_port,
                                         std::uint32_t message_id,
                                         std::uint32_t receive_size,
                                         const ConnectMethodResult &result) {
  using namespace iokit_abi::connect_method;
  const auto scalar_count = static_cast<std::uint32_t>(
      std::min<std::size_t>(result.scalar_output.size(), maximum_scalar_count));
  const auto inband_count = static_cast<std::uint32_t>(
      std::min<std::size_t>(result.inband_output.size(), maximum_inband_size));
  const auto encoded_reply_size = reply_size(scalar_count, inband_count);
  if (encoded_reply_size > receive_size)
    return mach_rcv_invalid_data;
  const std::vector<std::byte> cleared_reply(encoded_reply_size);
  if (!memory.copy_in(address, cleared_reply))
    return mach_rcv_invalid_data;
  const auto write = [&](std::uint32_t offset, std::uint32_t value) {
    return memory.write32(address + offset, value);
  };
  if (!write(darwin::mig_wire::header_bits_offset, mach_reply_bits) ||
      !write(darwin::mig_wire::header_size_offset, encoded_reply_size) ||
      !write(darwin::mig_wire::header_remote_port_offset, local_port) ||
      !write(darwin::mig_wire::header_identifier_offset,
             message_id + mig_reply_id_delta) ||
      !write(darwin::mig_wire::message_header_size, mach_ndr_native) ||
      !write(darwin::mig_wire::message_header_size +
                 darwin::mig_wire::word_size,
             mach_ndr_little_endian) ||
      !write(return_code_offset, result.return_code) ||
      !write(scalar_output_count_offset, scalar_count)) {
    return mach_rcv_invalid_data;
  }
  for (std::uint32_t index = 0; index < scalar_count; ++index) {
    const auto scalar_offset = scalar_output_offset + index * scalar_size;
    if (!write(scalar_offset,
               static_cast<std::uint32_t>(result.scalar_output[index])) ||
        !write(
            scalar_offset + sizeof(std::uint32_t),
            static_cast<std::uint32_t>(result.scalar_output[index] >> 32U))) {
      return mach_rcv_invalid_data;
    }
  }
  if (!write(inband_output_count_offset(scalar_count), inband_count)) {
    return mach_rcv_invalid_data;
  }
  if (inband_count != 0 &&
      !memory.copy_in(address + inband_output_offset(scalar_count),
                      std::span<const std::byte>{result.inband_output.data(),
                                                 inband_count})) {
    return mach_rcv_invalid_data;
  }
  if (!write(out_of_line_output_size_offset(scalar_count, inband_count), 0)) {
    return mach_rcv_invalid_data;
  }
  return 0;
}

bool contains_text(std::span<const std::byte> bytes, std::string_view text) {
  if (text.empty() || bytes.size() < text.size())
    return false;
  return std::search(bytes.begin(), bytes.end(), text.begin(), text.end(),
                     [](std::byte byte, char character) {
                       return std::to_integer<unsigned char>(byte) ==
                              static_cast<unsigned char>(character);
                     }) != bytes.end();
}

std::vector<std::byte> bytes_from_string(std::string_view value) {
  std::vector<std::byte> bytes(value.size());
  std::transform(value.begin(), value.end(), bytes.begin(), [](char character) {
    return static_cast<std::byte>(character);
  });
  return bytes;
}

std::vector<std::byte>
platform_compatible_bytes(std::string_view board_config) {
  auto bytes = bytes_from_string(board_config);
  bytes.push_back(std::byte{0});
  const auto architecture = bytes_from_string(apple_arm_compatible);
  bytes.insert(bytes.end(), architecture.begin(), architecture.end());
  bytes.push_back(std::byte{0});
  return bytes;
}

std::map<std::string, KernelSharedState::IOKitRegistryProperty>
registry_root_properties(const KernelSharedState &shared_state) {
  std::map<std::string, KernelSharedState::IOKitRegistryProperty> properties;
  properties.emplace(
      std::string{compatible_property},
      KernelSharedState::IOKitRegistryProperty{
          KernelSharedState::IOKitRegistryProperty::Kind::Data,
          platform_compatible_bytes(shared_state.device_board_config)});
  return properties;
}

std::string xml_escape(std::span<const std::byte> value) {
  std::string escaped;
  for (const auto byte : value) {
    switch (static_cast<char>(std::to_integer<unsigned char>(byte))) {
    case '&':
      escaped += "&amp;";
      break;
    case '<':
      escaped += "&lt;";
      break;
    case '>':
      escaped += "&gt;";
      break;
    default:
      escaped.push_back(
          static_cast<char>(std::to_integer<unsigned char>(byte)));
      break;
    }
  }
  return escaped;
}

std::optional<std::string>
serialized_matching_string(std::span<const std::byte> matching,
                           std::string_view key) {
  std::string text;
  text.reserve(matching.size());
  for (const auto byte : matching) {
    const auto character =
        static_cast<char>(std::to_integer<unsigned char>(byte));
    if (character == '\0')
      break;
    text.push_back(character);
  }

  const auto key_marker = "<key>" + std::string{key} + "</key>";
  const auto key_offset = text.find(key_marker);
  if (key_offset == std::string::npos)
    return std::nullopt;
  const auto string_offset =
      text.find("<string", key_offset + key_marker.size());
  if (string_offset == std::string::npos)
    return std::nullopt;
  const auto value_offset = text.find('>', string_offset);
  if (value_offset == std::string::npos)
    return std::nullopt;
  const auto end_offset = text.find("</string>", value_offset + 1U);
  if (end_offset == std::string::npos)
    return std::nullopt;

  std::string value{
      text.substr(value_offset + 1U, end_offset - value_offset - 1U)};
  const std::array<std::pair<std::string_view, std::string_view>, 3>
      replacements{{{"&amp;", "&"}, {"&lt;", "<"}, {"&gt;", ">"}}};
  for (const auto &[encoded, decoded] : replacements) {
    std::size_t offset = 0;
    while ((offset = value.find(encoded, offset)) != std::string::npos) {
      value.replace(offset, encoded.size(), decoded);
      offset += decoded.size();
    }
  }
  return value;
}

std::string base64_encode(std::span<const std::byte> value) {
  constexpr std::string_view alphabet{
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/"};
  std::string encoded;
  encoded.reserve(((value.size() + 2U) / 3U) * 4U);
  for (std::size_t offset = 0; offset < value.size(); offset += 3U) {
    const auto first = std::to_integer<unsigned>(value[offset]);
    const auto second = offset + 1U < value.size()
                            ? std::to_integer<unsigned>(value[offset + 1U])
                            : 0U;
    const auto third = offset + 2U < value.size()
                           ? std::to_integer<unsigned>(value[offset + 2U])
                           : 0U;
    const auto combined = (first << 16U) | (second << 8U) | third;
    encoded.push_back(alphabet[(combined >> 18U) & 0x3fU]);
    encoded.push_back(alphabet[(combined >> 12U) & 0x3fU]);
    encoded.push_back(
        offset + 1U < value.size() ? alphabet[(combined >> 6U) & 0x3fU] : '=');
    encoded.push_back(offset + 2U < value.size() ? alphabet[combined & 0x3fU]
                                                 : '=');
  }
  return encoded;
}

std::string
serialize_property_xml(const KernelSharedState::IOKitRegistryProperty &property,
                       std::size_t &identifier);

std::string serialize_dictionary_xml(
    const std::map<std::string, KernelSharedState::IOKitRegistryProperty>
        &properties,
    std::size_t &identifier) {
  std::string serialized{"<dict ID=\"" + std::to_string(identifier++) + "\">"};
  for (const auto &[name, property] : properties) {
    serialized += "<key>" + xml_escape(bytes_from_string(name)) + "</key>";
    serialized += serialize_property_xml(property, identifier);
  }
  serialized += "</dict>";
  return serialized;
}

std::string
serialize_property_xml(const KernelSharedState::IOKitRegistryProperty &property,
                       std::size_t &identifier) {
  using Kind = KernelSharedState::IOKitRegistryProperty::Kind;
  switch (property.kind) {
  case Kind::String:
    return "<string ID=\"" + std::to_string(identifier++) + "\">" +
           xml_escape(property.value) + "</string>";
  case Kind::Boolean:
    return !property.value.empty() && property.value.front() != std::byte{0}
               ? "<true/>"
               : "<false/>";
  case Kind::Number: {
    std::uint64_t value = 0;
    for (std::size_t index = 0;
         index < std::min(property.value.size(), sizeof(value)); ++index) {
      value |= static_cast<std::uint64_t>(property.value[index])
               << (index * 8U);
    }
    const auto bit_width =
        property.value.size() > sizeof(std::uint32_t) ? 64 : 32;
    std::ostringstream stream;
    stream << "<integer size=\"" << bit_width << "\" ID=\"" << identifier++
           << "\">0x" << std::hex << value << "</integer>";
    return stream.str();
  }
  case Kind::Array: {
    std::string serialized{"<array ID=\"" + std::to_string(identifier++) +
                           "\">"};
    for (const auto &element : property.array_value)
      serialized += serialize_property_xml(element, identifier);
    serialized += "</array>";
    return serialized;
  }
  case Kind::Dictionary:
    return serialize_dictionary_xml(property.dictionary_value, identifier);
  case Kind::Data:
    return "<data ID=\"" + std::to_string(identifier++) + "\">" +
           base64_encode(property.value) + "</data>";
  }
  return {};
}

std::vector<std::byte>
serialize_property(const KernelSharedState::IOKitRegistryProperty &property) {
  std::size_t identifier = 0;
  auto serialized = serialize_property_xml(property, identifier);
  // Darwin 8 OSSerialize::getLength() includes its trailing NUL.
  serialized.push_back('\0');
  return bytes_from_string(serialized);
}

std::vector<std::byte> serialize_properties(
    const std::map<std::string, KernelSharedState::IOKitRegistryProperty>
        &properties) {
  std::size_t identifier = 0;
  auto serialized = serialize_dictionary_xml(properties, identifier);
  serialized.push_back('\0');
  return bytes_from_string(serialized);
}

bool trace_repeated_call(std::uint64_t count) {
  constexpr std::uint64_t initial_trace_count = 16;
  return count <= initial_trace_count ||
         (count != 0 && (count & (count - 1U)) == 0);
}

std::string_view
user_client_profile_name(KernelSharedState::IOKitUserClientProfile profile) {
  switch (profile) {
  case KernelSharedState::IOKitUserClientProfile::Generic:
    return "generic";
  case KernelSharedState::IOKitUserClientProfile::SerialMultiplexer:
    return "serial-multiplexer";
  case KernelSharedState::IOKitUserClientProfile::Display:
    return "display";
  case KernelSharedState::IOKitUserClientProfile::CoreSurface:
    return "core-surface";
  case KernelSharedState::IOKitUserClientProfile::Mbx:
    return "mbx";
  case KernelSharedState::IOKitUserClientProfile::GraphicsAccelerator:
    return "graphics-accelerator";
  case KernelSharedState::IOKitUserClientProfile::CameraSensor:
    return "camera-sensor";
  case KernelSharedState::IOKitUserClientProfile::CameraAccelerator:
    return "camera-accelerator";
  case KernelSharedState::IOKitUserClientProfile::JpegAccelerator:
    return "jpeg-accelerator";
  case KernelSharedState::IOKitUserClientProfile::Audio:
    return "audio";
  case KernelSharedState::IOKitUserClientProfile::MobileFileIntegrity:
    return "mobile-file-integrity";
  case KernelSharedState::IOKitUserClientProfile::None:
    break;
  }
  return "none";
}

std::uint32_t
connection_type_for_profile(KernelSharedState::IOKitUserClientProfile profile,
                            std::uint32_t requested_type) {
  if (profile == KernelSharedState::IOKitUserClientProfile::Display)
    return iokit_abi::apple_h1clcd_service_type;
  if (profile == KernelSharedState::IOKitUserClientProfile::CoreSurface)
    return iokit_abi::core_surface_root_service_type;
  if (profile == KernelSharedState::IOKitUserClientProfile::Audio)
    return kernel_iokit::audio::IOKitAudioAbiProfile::io_audio2().service_type;
  if (profile == KernelSharedState::IOKitUserClientProfile::Generic ||
      profile == KernelSharedState::IOKitUserClientProfile::SerialMultiplexer)
    return iokit_abi::generic_user_client_type;
  return requested_type;
}

std::uint32_t write_status_reply(AddressSpace &memory, std::uint32_t address,
                                 std::uint32_t local_port,
                                 std::uint32_t message_id,
                                 std::uint32_t result) {
  const std::array<std::uint32_t, 9> reply{
      mach_reply_bits,
      36U,
      local_port,
      0,
      0,
      message_id + mig_reply_id_delta,
      mach_ndr_native,
      mach_ndr_little_endian,
      result,
  };
  return write_reply(memory, address, reply);
}

std::string format_call_site(AddressSpace &memory,
                             const IOKitMachCallSite &call_site) {
  if (call_site.program_counter == 0 && call_site.link_register == 0 &&
      call_site.frame_pointer == 0) {
    return {};
  }
  std::ostringstream trace;
  trace << std::hex << " pc=0x" << call_site.program_counter << " lr=0x"
        << call_site.link_register << " frames=";
  constexpr std::size_t maximum_frames = 8;
  auto frame = call_site.frame_pointer;
  for (std::size_t depth = 0; depth < maximum_frames && frame != 0; ++depth) {
    const auto previous = memory.read32(frame);
    const auto return_address = memory.read32(frame + sizeof(std::uint32_t));
    if (!previous || !return_address)
      break;
    if (depth != 0)
      trace << ',';
    trace << "0x" << *return_address;
    if (*previous == frame)
      break;
    frame = *previous;
  }
  return trace.str();
}

std::string matching_hex(std::span<const std::byte> matching) {
  constexpr std::string_view digits{"0123456789abcdef"};
  const auto size = std::min(matching.size(), maximum_matching_trace_bytes);
  std::string result;
  result.reserve(size * 2U);
  for (std::size_t index = 0; index < size; ++index) {
    const auto value = std::to_integer<unsigned>(matching[index]);
    result.push_back(digits[value >> 4U]);
    result.push_back(digits[value & 0xfU]);
  }
  return result;
}

std::uint32_t
ensure_mobile_framebuffer_service_locked(KernelSharedState &shared_state) {
  if (shared_state.mobile_framebuffer_service != 0) {
    return shared_state.mobile_framebuffer_service;
  }
  const auto port = shared_state.allocate_mach_object();
  shared_state.mobile_framebuffer_service = port;
  static_cast<void>(shared_state.mach_port_objects.create(port));
  shared_state.mach_queues.try_emplace(port);
  shared_state.iokit_services.emplace(
      port, KernelSharedState::IOKitService{
                std::string{apple_h1clcd_class},
                {std::string{mobile_framebuffer_class}},
                {},
                {},
                0,
                KernelSharedState::IOKitUserClientProfile::Display});
  return port;
}

std::uint32_t
ensure_core_surface_root_service_locked(KernelSharedState &shared_state) {
  const auto existing =
      std::find_if(shared_state.iokit_services.begin(),
                   shared_state.iokit_services.end(), [](const auto &entry) {
                     return entry.second.class_name == core_surface_root_class;
                   });
  if (existing != shared_state.iokit_services.end())
    return existing->first;

  const auto object = shared_state.allocate_mach_object();
  static_cast<void>(shared_state.mach_port_objects.create(object));
  shared_state.mach_queues.try_emplace(object);
  shared_state.iokit_services.emplace(
      object, KernelSharedState::IOKitService{
                  std::string{core_surface_root_class},
                  {"IOService"},
                  {},
                  "IOService:/IOCoreSurfaceRoot",
                  0,
                  KernelSharedState::IOKitUserClientProfile::CoreSurface});
  return object;
}

std::uint32_t
ensure_platform_expert_service_locked(KernelSharedState &shared_state) {
  const auto existing =
      std::find_if(shared_state.iokit_services.begin(),
                   shared_state.iokit_services.end(), [](const auto &entry) {
                     return entry.second.class_name == platform_expert_class;
                   });
  if (existing != shared_state.iokit_services.end())
    return existing->first;

  const auto object = shared_state.allocate_mach_object();
  static_cast<void>(shared_state.mach_port_objects.create(object));
  shared_state.mach_queues.try_emplace(object);

  KernelSharedState::IOKitService service{
      std::string{platform_expert_class}, {"IOService"}, {}};
  service.properties.emplace(
      std::string{model_number_property},
      KernelSharedState::IOKitRegistryProperty{
          KernelSharedState::IOKitRegistryProperty::Kind::Data,
          bytes_from_string(shared_state.device_model_number)});
  // Darwin 8's early Lockdown path calls CFRetain on the result of the
  // platform serial query without first handling kIOReturnNotFound. The
  // audited ABI contract records whether that legacy property is present;
  // unknown firmware remains conservative.
  if (shared_state.darwin_kernel_identity.capabilities
          .expose_legacy_platform_serial) {
    const auto serial = std::string{"iLEmu-"} +
                        shared_state.device_product_type + "-" +
                        shared_state.device_model_number;
    service.properties.emplace(
        std::string{platform_serial_number_property},
        KernelSharedState::IOKitRegistryProperty{
            KernelSharedState::IOKitRegistryProperty::Kind::String,
            bytes_from_string(serial)});
  }
  shared_state.iokit_services.emplace(object, std::move(service));
  return object;
}

std::uint32_t
ensure_usb_device_mux_service_locked(KernelSharedState &shared_state) {
  const auto existing =
      std::find_if(shared_state.iokit_services.begin(),
                   shared_state.iokit_services.end(), [](const auto &entry) {
                     return entry.second.class_name == io_ipod_usb_device_class;
                   });
  if (existing != shared_state.iokit_services.end())
    return existing->first;

  const auto parent = ensure_platform_expert_service_locked(shared_state);
  const auto object = shared_state.allocate_mach_object();
  static_cast<void>(shared_state.mach_port_objects.create(object));
  shared_state.mach_queues.try_emplace(object);
  shared_state.iokit_services.emplace(
      object, KernelSharedState::IOKitService{
                  std::string{io_ipod_usb_device_class},
                  {"IOService"},
                  {},
                  "IOService:/IOPlatformExpertDevice/IOIpodUSBDevice",
                  parent});
  return object;
}

std::uint32_t ensure_wifi_bus_service_locked(KernelSharedState &shared_state) {
  const auto existing =
      std::find_if(shared_state.iokit_services.begin(),
                   shared_state.iokit_services.end(), [](const auto &entry) {
                     return entry.second.class_name == wifi_bus_class;
                   });
  if (existing != shared_state.iokit_services.end())
    return existing->first;

  const auto object = shared_state.allocate_mach_object();
  static_cast<void>(shared_state.mach_port_objects.create(object));
  shared_state.mach_queues.try_emplace(object);
  KernelSharedState::IOKitService service{std::string{wifi_bus_class},
                                          {"IOService"},
                                          {},
                                          "IOService:/AppleARMIODevice"};
  service.properties.emplace(
      "name", KernelSharedState::IOKitRegistryProperty{
                  KernelSharedState::IOKitRegistryProperty::Kind::String,
                  bytes_from_string(sdio_device_name)});
  service.properties.emplace(
      std::string{local_mac_address_property},
      KernelSharedState::IOKitRegistryProperty{
          KernelSharedState::IOKitRegistryProperty::Kind::Data,
          std::vector<std::byte>{wifi_interface_mac_address.begin(),
                                 wifi_interface_mac_address.end()}});
  shared_state.iokit_services.emplace(object, std::move(service));
  return object;
}

std::uint32_t ensure_wifi_service_locked(KernelSharedState &shared_state) {
  if (shared_state.wifi_service != 0)
    return shared_state.wifi_service;

  const auto bus_object = ensure_wifi_bus_service_locked(shared_state);
  const auto object = shared_state.allocate_mach_object();
  shared_state.wifi_service = object;
  static_cast<void>(shared_state.mach_port_objects.create(object));
  shared_state.mach_queues.try_emplace(object);

  KernelSharedState::IOKitService service{
      std::string{io80211_controller_class},
      {"IOService"},
      {},
      "IOService:/AppleARMIODevice/IO80211Controller",
      bus_object};
  service.properties.emplace(
      std::string{bsd_name_property},
      KernelSharedState::IOKitRegistryProperty{
          KernelSharedState::IOKitRegistryProperty::Kind::String,
          bytes_from_string("en0")});
  service.properties.emplace(
      std::string{network_root_type_property},
      KernelSharedState::IOKitRegistryProperty{
          KernelSharedState::IOKitRegistryProperty::Kind::String,
          bytes_from_string("airport")});
  service.properties.emplace(
      std::string{mac_address_property},
      KernelSharedState::IOKitRegistryProperty{
          KernelSharedState::IOKitRegistryProperty::Kind::Data,
          std::vector<std::byte>{wifi_interface_mac_address.begin(),
                                 wifi_interface_mac_address.end()}});
  shared_state.iokit_services.emplace(object, std::move(service));
  return object;
}

std::uint32_t
ensure_network_stack_service_locked(KernelSharedState &shared_state) {
  const auto existing =
      std::find_if(shared_state.iokit_services.begin(),
                   shared_state.iokit_services.end(), [](const auto &entry) {
                     return entry.second.class_name == io_network_stack_class;
                   });
  if (existing != shared_state.iokit_services.end())
    return existing->first;

  const auto object = shared_state.allocate_mach_object();
  static_cast<void>(shared_state.mach_port_objects.create(object));
  shared_state.mach_queues.try_emplace(object);
  shared_state.iokit_services.emplace(
      object, KernelSharedState::IOKitService{
                  std::string{io_network_stack_class},
                  {"IOService"},
                  {},
                  "IOService:/IONetworkStack",
                  0,
                  KernelSharedState::IOKitUserClientProfile::Generic});
  return object;
}

std::uint32_t
ensure_wifi_interface_service_locked(KernelSharedState &shared_state) {
  if (shared_state.wifi_interface_service != 0)
    return shared_state.wifi_interface_service;

  const auto controller_object = ensure_wifi_service_locked(shared_state);
  const auto object = shared_state.allocate_mach_object();
  shared_state.wifi_interface_service = object;
  static_cast<void>(shared_state.mach_port_objects.create(object));
  shared_state.mach_queues.try_emplace(object);

  KernelSharedState::IOKitService service{
      std::string{io80211_interface_class},
      {std::string{io_ethernet_interface_class},
       std::string{io_network_interface_class}, "IOService"},
      {},
      "IOService:/AppleARMIODevice/IO80211Controller/IO80211Interface",
      controller_object};
  service.properties.emplace(
      std::string{bsd_name_property},
      KernelSharedState::IOKitRegistryProperty{
          KernelSharedState::IOKitRegistryProperty::Kind::String,
          bytes_from_string("en0")});
  service.properties.emplace(
      std::string{interface_name_prefix_property},
      KernelSharedState::IOKitRegistryProperty{
          KernelSharedState::IOKitRegistryProperty::Kind::String,
          bytes_from_string("en")});
  service.properties.emplace(
      std::string{interface_unit_property},
      KernelSharedState::IOKitRegistryProperty{
          KernelSharedState::IOKitRegistryProperty::Kind::Number,
          {std::byte{0}, std::byte{0}, std::byte{0}, std::byte{0}}});
  service.properties.emplace(
      std::string{interface_type_property},
      KernelSharedState::IOKitRegistryProperty{
          KernelSharedState::IOKitRegistryProperty::Kind::Number,
          {std::byte{6}, std::byte{0}, std::byte{0}, std::byte{0}}});
  service.properties.emplace(
      std::string{network_root_type_property},
      KernelSharedState::IOKitRegistryProperty{
          KernelSharedState::IOKitRegistryProperty::Kind::String,
          bytes_from_string("airport")});
  const auto true_property = KernelSharedState::IOKitRegistryProperty{
      KernelSharedState::IOKitRegistryProperty::Kind::Boolean, {std::byte{1}}};
  service.properties.emplace(std::string{builtin_interface_property},
                             true_property);
  service.properties.emplace(std::string{primary_interface_property},
                             true_property);
  service.properties.emplace(
      std::string{mac_address_property},
      KernelSharedState::IOKitRegistryProperty{
          KernelSharedState::IOKitRegistryProperty::Kind::Data,
          std::vector<std::byte>{wifi_interface_mac_address.begin(),
                                 wifi_interface_mac_address.end()}});
  shared_state.iokit_services.emplace(object, std::move(service));
  return object;
}

std::uint32_t resolve_task_name_locked(const KernelSharedState &shared_state,
                                       std::uint32_t task, std::uint32_t name) {
  if (const auto object = shared_state.mach_namespaces.resolve(task, name)) {
    return *object;
  }
  // Standalone ABI tests may invoke this module without constructing a
  // CompatibilityKernel. A live guest task always has an ipc_space and an
  // unknown name must not accidentally address a same-valued kernel object.
  return shared_state.mach_namespaces.contains_task(task) ? 0U : name;
}

std::uint32_t copyout_send_locked(KernelSharedState &shared_state,
                                  std::uint32_t task, std::uint32_t object) {
  if (object == xnu792::ipc::null_name)
    return xnu792::ipc::null_name;
  return shared_state.mach_namespaces
      .copyout(task, object, xnu792::ipc::type_mask(xnu792::ipc::Right::Send))
      .value_or(xnu792::ipc::null_name);
}

void populate_matching_services_locked(KernelSharedState &shared_state,
                                       std::span<const std::byte> matching,
                                       std::deque<std::uint32_t> &services) {
  if (const auto bsd_name =
          serialized_matching_string(matching, bsd_name_property)) {
    if (shared_state.wifi_service_available) {
      static_cast<void>(ensure_wifi_interface_service_locked(shared_state));
    }
    for (const auto &[object, service] : shared_state.iokit_services) {
      const auto property =
          service.properties.find(std::string{bsd_name_property});
      if (property == service.properties.end() ||
          property->second.kind !=
              KernelSharedState::IOKitRegistryProperty::Kind::String) {
        continue;
      }
      const auto &value = property->second.value;
      if (value.size() == bsd_name->size() &&
          std::equal(value.begin(), value.end(), bsd_name->begin(),
                     [](std::byte byte, char character) {
                       return std::to_integer<unsigned char>(byte) ==
                              static_cast<unsigned char>(character);
                     })) {
        services.push_back(object);
      }
    }
    return;
  }
  if (contains_text(matching, platform_expert_class)) {
    services.push_back(ensure_platform_expert_service_locked(shared_state));
  }
  if (kernel_iokit::graphics::matches_service(matching) &&
      shared_state.graphics_accelerator ==
          GraphicsAcceleratorProfileKind::Sgx535 &&
      !shared_state.graphics_driver_bundle.empty()) {
    const auto platform_expert =
        ensure_platform_expert_service_locked(shared_state);
    services.push_back(kernel_iokit::graphics::ensure_service_locked(
        shared_state, platform_expert));
  }
  if (kernel_iokit::mbx::matches_service(matching) &&
      shared_state.graphics_accelerator ==
          GraphicsAcceleratorProfileKind::MbxLite) {
    const auto platform_expert =
        ensure_platform_expert_service_locked(shared_state);
    services.push_back(kernel_iokit::mbx::ensure_service_locked(
        shared_state, platform_expert));
  }
  if (const auto profile =
          kernel_iokit::baseband::matching_service(matching);
      profile && shared_state.baseband_device_state.available()) {
    // Offline keeps the registry-level service visible so CoreTelephony can
    // observe the modem's normal Offline state. The filesystem boundary
    // rejects the mux transport separately; this service must not be treated
    // as evidence that a physical modem or a data path exists.
    services.push_back(kernel_iokit::baseband::ensure_service_locked(
        shared_state, *profile));
  }
  if (kernel_iokit::audio::matches_service(matching)) {
    const auto audio_services =
        kernel_iokit::audio::ensure_services_locked(shared_state);
    services.insert(services.end(), audio_services.begin(),
                    audio_services.end());
  }
  if (kernel_iokit::camera::matches_sensor_service(matching)) {
    const auto platform_expert =
        ensure_platform_expert_service_locked(shared_state);
    services.push_back(kernel_iokit::camera::ensure_sensor_service_locked(
        shared_state, platform_expert));
  }
  if (const auto service_class =
          kernel_iokit::camera::matching_accelerator_service(matching)) {
    const auto platform_expert =
        ensure_platform_expert_service_locked(shared_state);
    services.push_back(
        kernel_iokit::camera::ensure_accelerator_service_locked(
            shared_state, platform_expert, *service_class));
  }
  if (kernel_iokit::jpeg::matches_service(matching)) {
    const auto platform_expert =
        ensure_platform_expert_service_locked(shared_state);
    services.push_back(kernel_iokit::jpeg::ensure_service_locked(
        shared_state, platform_expert));
  }
  if (contains_text(matching, io_ipod_usb_device_class)) {
    services.push_back(ensure_usb_device_mux_service_locked(shared_state));
  }
  if (shared_state.wifi_service_available &&
      contains_text(matching, io80211_controller_class)) {
    services.push_back(ensure_wifi_service_locked(shared_state));
  }
  if (shared_state.wifi_service_available &&
      (contains_text(matching, io80211_interface_class) ||
       contains_text(matching, io_ethernet_interface_class) ||
       contains_text(matching, io_network_interface_class))) {
    services.push_back(ensure_wifi_interface_service_locked(shared_state));
  }
  // IONetworkStack represents the guest kernel's networking subsystem, not
  // an external Wi-Fi uplink. It remains published for loopback and local IPC
  // even when the host policy intentionally exposes no radio/backend.
  if (contains_text(matching, io_network_stack_class)) {
    services.push_back(ensure_network_stack_service_locked(shared_state));
  }
  if (shared_state.wifi_service_available &&
      contains_text(matching, sdio_device_name)) {
    services.push_back(ensure_wifi_bus_service_locked(shared_state));
  }
  // S5L8900/iPhone1,1 uses AppleH1CLCD. AppleMX31IPU belongs to a different
  // first-generation platform and intentionally remains unmatched.
  if (contains_text(matching, apple_h1clcd_class) ||
      contains_text(matching, mobile_framebuffer_class)) {
    services.push_back(ensure_mobile_framebuffer_service_locked(shared_state));
  }
  if (contains_text(matching, core_surface_root_class)) {
    services.push_back(ensure_core_surface_root_service_locked(shared_state));
  }
  if (kernel_iokit::mobile_file_integrity::matches_service(matching)) {
    services.push_back(
        kernel_iokit::mobile_file_integrity::ensure_service_locked(
            shared_state));
  }
}

template <std::size_t Size>
std::uint32_t write_reply(AddressSpace &memory, std::uint32_t address,
                          const std::array<std::uint32_t, Size> &reply) {
  for (std::size_t index = 0; index < reply.size(); ++index) {
    if (!memory.write32(address + static_cast<std::uint32_t>(index * 4U),
                        reply[index])) {
      return mach_rcv_invalid_data;
    }
  }
  return 0;
}

std::uint32_t write_port_reply(AddressSpace &memory, std::uint32_t address,
                               std::uint32_t local_port,
                               std::uint32_t message_id, std::uint32_t port) {
  const std::array<std::uint32_t, 10> reply{
      0x80000012U,      40, local_port, 0, 0,
      message_id + 100, 1,  port,       0, 0x00110000U,
  };
  return write_reply(memory, address, reply);
}

std::optional<KernelSharedState::IOKitUserClientProfile>
service_user_client_profile_locked(const KernelSharedState &shared_state,
                                   std::uint32_t service_object) {
  const auto service = shared_state.iokit_services.find(service_object);
  if (service == shared_state.iokit_services.end())
    return std::nullopt;
  return service->second.user_client_profile;
}

std::optional<KernelSharedState::IOKitUserClientProfile>
connection_user_client_profile_locked(const KernelSharedState &shared_state,
                                      std::uint32_t connection_object) {
  const auto connection =
      shared_state.iokit_connections.find(connection_object);
  if (connection == shared_state.iokit_connections.end())
    return std::nullopt;
  return service_user_client_profile_locked(shared_state,
                                            connection->second.service_port);
}

} // namespace

std::optional<std::uint32_t> handle_iokit_mach_request(
    AddressSpace &memory, Output &output, KernelSharedState &shared_state,
    ProcessContext &process, std::uint32_t message_id,
    std::uint32_t message_address, std::uint32_t send_size,
    std::uint32_t receive_size, std::uint32_t remote_port,
    std::uint32_t local_port, IOKitMachCallSite call_site,
    SurfaceStore *surfaces) {
  std::uint32_t remote_object = 0;
  {
    std::lock_guard mach_lock{shared_state.mach_mutex};
    remote_object =
        resolve_task_name_locked(shared_state, process.pid, remote_port);
  }
  if (remote_object == xnu792::ipc::null_name) {
    if (message_id == static_cast<std::uint32_t>(
                          iokit_abi::Message::ConnectSetNotificationPort) ||
        message_id ==
            static_cast<std::uint32_t>(iokit_abi::Message::ConnectMethod)) {
      output.write(
          "[iokit] invalid-connection pid=" + std::to_string(process.pid) +
          " connection-name=" + std::to_string(remote_port) +
          " id=" + std::to_string(message_id) + "\n");
    }
    return std::nullopt;
  }
  if (const auto power_result = kernel_iokit::handle_power_mach_request(
          memory, output, shared_state, process, message_id, message_address,
          receive_size, remote_object, local_port)) {
    return power_result;
  }
  if (const auto display_result =
          kernel_iokit::display::handle_notification_port_request(
              memory, output, shared_state, process, message_id,
              message_address, send_size, receive_size, remote_object,
              local_port)) {
    return display_result;
  }
  if (const auto audio_result = kernel_iokit::audio::handle_mach_request(
          memory, output, shared_state, process, message_id, message_address,
          send_size, receive_size, remote_object, local_port)) {
    return audio_result;
  }
  if (const auto camera_result =
          kernel_iokit::camera::handle_notification_port_request(
              memory, output, shared_state, process, message_id,
              message_address, send_size, receive_size, remote_object,
              local_port)) {
    return camera_result;
  }
  if (message_id == static_cast<std::uint32_t>(
                        iokit_abi::Message::ConnectSetNotificationPort)) {
    std::optional<KernelSharedState::IOKitUserClientProfile> profile;
    {
      std::lock_guard mach_lock{shared_state.mach_mutex};
      profile =
          connection_user_client_profile_locked(shared_state, remote_object);
    }
    if (profile &&
        *profile != KernelSharedState::IOKitUserClientProfile::Display) {
      output.write(
          "[iokit] notification-port pid=" + std::to_string(process.pid) +
          " connection-object=" + std::to_string(remote_object) +
          " profile=" + std::string{user_client_profile_name(*profile)} +
          " result=unsupported\n");
      return write_status_reply(memory, message_address, local_port, message_id,
                                iokit_abi::unsupported);
    }
  }
  if (const auto graphics_result = kernel_iokit::graphics::handle_mach_request(
          memory, shared_state, process, message_id, message_address,
          send_size, receive_size, remote_object, local_port)) {
    return *graphics_result;
  }
  if (message_id ==
      static_cast<std::uint32_t>(iokit_abi::Message::ConnectMapMemory)) {
    std::optional<KernelSharedState::IOKitUserClientProfile> profile;
    {
      std::lock_guard mach_lock{shared_state.mach_mutex};
      profile =
          connection_user_client_profile_locked(shared_state, remote_object);
    }
    if (profile &&
        *profile != KernelSharedState::IOKitUserClientProfile::Display) {
      output.write(
          "[iokit] map-memory pid=" + std::to_string(process.pid) +
          " connection-object=" + std::to_string(remote_object) +
          " profile=" + std::string{user_client_profile_name(*profile)} +
          " result=unsupported\n");
      return write_status_reply(memory, message_address, local_port, message_id,
                                iokit_abi::unsupported);
    }
  }
  if (message_id == device_mig::id(device_mig::Routine::io_make_matching)) {
    std::array<std::uint32_t, make_matching_arguments.size()> element_counts{};
    element_counts[3] =
        memory
            .read32(message_address +
                    make_matching_arguments[3].request_count_offset)
            .value_or(make_matching_arguments[3].wire_size + 1U);
    const auto request_layout = xnu792::mig::compute_wire_layout(
        make_matching_arguments, xnu792::mig::WireLayoutSide::Request,
        element_counts);
    if (!request_layout || element_counts[3] == 0 ||
        element_counts[3] > make_matching_arguments[3].wire_size ||
        (*request_layout)[3].offset + element_counts[3] > send_size) {
      return mach_rcv_invalid_data;
    }
    const auto type =
        memory.read32(message_address + (*request_layout)[1].offset)
            .value_or(0);
    const auto input_bytes = memory.read_bytes(
        message_address + (*request_layout)[3].offset, element_counts[3]);
    if (!input_bytes)
      return mach_rcv_invalid_data;

    std::span<const std::byte> input{*input_bytes};
    if (const auto terminator =
            std::find(input.begin(), input.end(), std::byte{0});
        terminator != input.end()) {
      input = input.first(
          static_cast<std::size_t>(std::distance(input.begin(), terminator)));
    }
    std::string matching{"<dict ID=\"0\">"};
    if (type == io_service_matching_type) {
      matching +=
          "<key>IOProviderClass</key><string ID=\"1\">IOService</string>";
    } else if (type == io_bsd_name_matching_type) {
      matching +=
          "<key>IOProviderClass</key><string ID=\"1\">IOService</string>"
          "<key>BSD Name</key><string ID=\"2\">" +
          xml_escape(input) + "</string>";
    } else if (type == io_of_path_matching_type) {
      matching += "<key>IOPathMatch</key><string ID=\"1\">IODeviceTree:" +
                  xml_escape(input) + "</string>";
    } else {
      constexpr std::uint32_t reply_size = 36U;
      if (receive_size < reply_size)
        return mach_rcv_invalid_data;
      const std::array<std::uint32_t, reply_size / sizeof(std::uint32_t)> reply{
          mach_reply_bits,
          reply_size,
          local_port,
          0,
          0,
          message_id + mig_reply_id_delta,
          mach_ndr_native,
          mach_ndr_little_endian,
          iokit_abi::unsupported};
      return write_reply(memory, message_address, reply);
    }
    matching += "</dict>";
    matching.push_back('\0');
    if (matching.size() > make_matching_arguments[4].wire_size)
      return mach_rcv_invalid_data;

    const auto matching_count = static_cast<std::uint32_t>(matching.size());
    element_counts.fill(0);
    element_counts[4] = matching_count;
    const auto reply_layout = xnu792::mig::compute_wire_layout(
        make_matching_arguments, xnu792::mig::WireLayoutSide::Reply,
        element_counts);
    if (!reply_layout)
      return mach_rcv_invalid_data;
    const auto reply_size =
        (*reply_layout)[4].offset + align_mig_field(matching_count);
    if (reply_size > receive_size)
      return mach_rcv_invalid_data;
    const std::vector<std::byte> cleared_reply(reply_size);
    if (!memory.copy_in(message_address, cleared_reply))
      return mach_rcv_invalid_data;
    const auto type_word =
        8U | (8U << 8U) |
        (static_cast<std::uint32_t>(make_matching_arguments[4].wire_size)
         << 16U) |
        (1U << 28U);
    const std::array<std::uint32_t, 11> reply{mach_reply_bits,
                                              reply_size,
                                              local_port,
                                              0,
                                              0,
                                              message_id + mig_reply_id_delta,
                                              mach_ndr_native,
                                              mach_ndr_little_endian,
                                              iokit_abi::success,
                                              type_word,
                                              matching_count};
    if (write_reply(memory, message_address, reply) != 0 ||
        !memory.copy_in(
            message_address + (*reply_layout)[4].offset,
            std::span<const std::byte>{
                reinterpret_cast<const std::byte *>(matching.data()),
                matching.size()})) {
      return mach_rcv_invalid_data;
    }
    return 0;
  }

  if (message_id == static_cast<std::uint32_t>(
                        iokit_abi::Message::RegistryGetRootEntry)) {
    if (receive_size < 40U)
      return mach_rcv_invalid_data;
    std::uint32_t root_object = xnu792::ipc::null_name;
    std::uint32_t root_name = xnu792::ipc::null_name;
    {
      std::lock_guard mach_lock{shared_state.mach_mutex};
      root_object = shared_state.iokit_registry_root_object;
      root_name = copyout_send_locked(shared_state, process.pid, root_object);
    }
    output.write("[iokit] root-entry pid=" + std::to_string(process.pid) +
                 " master-object=" + std::to_string(remote_object) +
                 " root-object=" + std::to_string(root_object) +
                 " root-name=" + std::to_string(root_name) + "\n");
    return write_port_reply(memory, message_address, local_port, message_id,
                            root_name);
  }

  if (message_id == static_cast<std::uint32_t>(
                        iokit_abi::Message::ServiceGetMatchingServices)) {
    // io_service_get_matching_services uses the Darwin 8 c_string wire
    // form generated from device.defs. Until a modeled service matches,
    // return a valid empty iterator so callers take their no-hardware path.
    // receive_size is the mach_msg receive capacity, not the request's
    // send size. Validate only the minimum port reply here; AddressSpace
    // reads below validate the independently sized request buffer.
    if (receive_size < 40U)
      return mach_rcv_invalid_data;
    const auto matching_count =
        memory
            .read32(message_address +
                    matching_services_matching.request_count_offset)
            .value_or(0);
    const auto matching =
        matching_count != 0 && matching_count <= 512U
            ? memory.read_bytes(message_address +
                                    matching_services_matching.request_offset,
                                matching_count)
            : std::nullopt;
    if (!matching)
      return mach_rcv_invalid_data;

    std::uint32_t iterator_object = 0;
    std::uint32_t iterator_name = 0;
    std::size_t matching_service_count = 0;
    {
      std::lock_guard mach_lock{shared_state.mach_mutex};
      iterator_object = shared_state.allocate_mach_object();
      static_cast<void>(shared_state.mach_port_objects.create(iterator_object));
      auto [iterator, inserted] =
          shared_state.iokit_iterators.try_emplace(iterator_object);
      static_cast<void>(inserted);
      populate_matching_services_locked(shared_state, *matching,
                                        iterator->second);
      matching_service_count = iterator->second.size();
      iterator_name =
          copyout_send_locked(shared_state, process.pid, iterator_object);
    }
    output.write("[iokit] matching pid=" + std::to_string(process.pid) +
                 " bytes=" + std::to_string(matching->size()) +
                 " iterator-name=" + std::to_string(iterator_name) +
                 " iterator-object=" + std::to_string(iterator_object) +
                 " matches=" + std::to_string(matching_service_count) +
                 (matching_service_count == 0
                      ? " matching-hex=" + matching_hex(*matching)
                      : "") +
                 "\n");
    return write_port_reply(memory, message_address, local_port, message_id,
                            iterator_name);
  }

  if (message_id ==
      static_cast<std::uint32_t>(iokit_abi::Message::ServiceAddNotification)) {
    // io_service_add_notification. iPhoneOS 1.0 uses routine 0xb21 for
    // the inline matching form. The returned iterator contains services
    // which already matched when notification registration completed.
    if (receive_size < 40U)
      return mach_rcv_invalid_data;
    const auto descriptor_count =
        memory
            .read32(message_address +
                    darwin::mig_wire::complex_descriptor_count_offset)
            .value_or(0);
    const auto notification_port =
        memory.read32(message_address + notification_wake_port.request_offset)
            .value_or(0);
    const auto type_count =
        memory.read32(message_address + notification_type.request_count_offset)
            .value_or(0);
    std::array<std::uint32_t,
               device_mig::io_service_add_notification_arguments.size()>
        element_counts{};
    element_counts[1] = type_count;
    const auto type_layout = xnu792::mig::compute_wire_layout(
        device_mig::io_service_add_notification_arguments,
        xnu792::mig::WireLayoutSide::Request, element_counts);
    if (!type_layout)
      return mach_rcv_invalid_data;
    const auto matching_count_offset = (*type_layout)[2].count_offset;
    const auto matching_count =
        memory.read32(message_address + matching_count_offset).value_or(0);
    element_counts[2] = matching_count;
    const auto matching_layout = xnu792::mig::compute_wire_layout(
        device_mig::io_service_add_notification_arguments,
        xnu792::mig::WireLayoutSide::Request, element_counts);
    if (!matching_layout)
      return mach_rcv_invalid_data;
    const auto reference_count =
        memory.read32(message_address + (*matching_layout)[4].count_offset)
            .value_or(0);
    element_counts[4] = reference_count;
    const auto complete_layout = xnu792::mig::compute_wire_layout(
        device_mig::io_service_add_notification_arguments,
        xnu792::mig::WireLayoutSide::Request, element_counts);
    if (!complete_layout)
      return mach_rcv_invalid_data;
    const auto type_bytes = memory.read_bytes(
        message_address + (*complete_layout)[1].offset, type_count);
    const auto matching_bytes = memory.read_bytes(
        message_address + (*complete_layout)[2].offset, matching_count);
    const auto reference_bytes = memory.read_bytes(
        message_address + (*complete_layout)[4].offset,
        reference_count * notification_reference.element_size);
    if (descriptor_count != 1 || !type_bytes || !matching_bytes ||
        !reference_bytes) {
      return mach_rcv_invalid_data;
    }

    std::uint32_t iterator_object = 0;
    std::uint32_t iterator_name = 0;
    std::size_t matching_service_count = 0;
    {
      std::lock_guard mach_lock{shared_state.mach_mutex};
      const auto notification_object = resolve_task_name_locked(
          shared_state, process.pid, notification_port);
      if (notification_object == xnu792::ipc::null_name) {
        return mach_rcv_invalid_data;
      }
      iterator_object = shared_state.allocate_mach_object();
      static_cast<void>(shared_state.mach_port_objects.create(iterator_object));
      auto [iterator, inserted] =
          shared_state.iokit_iterators.try_emplace(iterator_object);
      static_cast<void>(inserted);
      populate_matching_services_locked(shared_state, *matching_bytes,
                                        iterator->second);
      matching_service_count = iterator->second.size();
      std::string type;
      type.reserve(type_bytes->size());
      for (const auto byte : *type_bytes) {
        type.push_back(static_cast<char>(byte));
      }
      if (!type.empty() && type.back() == '\0')
        type.pop_back();
      shared_state.iokit_notifications.push_back(
          KernelSharedState::IOKitNotification{process.pid, notification_object,
                                               std::move(type),
                                               std::move(*matching_bytes)});
      iterator_name =
          copyout_send_locked(shared_state, process.pid, iterator_object);
    }
    output.write("[iokit] notification pid=" + std::to_string(process.pid) +
                 " port=" + std::to_string(notification_port) +
                 " iterator-name=" + std::to_string(iterator_name) +
                 " iterator-object=" + std::to_string(iterator_object) +
                 " matches=" + std::to_string(matching_service_count) +
                 " matching-hex=" + matching_hex(*matching_bytes) + "\n");
    return write_port_reply(memory, message_address, local_port, message_id,
                            iterator_name);
  }

  if (message_id ==
      static_cast<std::uint32_t>(iokit_abi::Message::IteratorNext)) {
    if (receive_size < 40)
      return mach_rcv_invalid_data;
    std::uint32_t object_port = 0;
    std::uint32_t object_name = 0;
    {
      std::lock_guard mach_lock{shared_state.mach_mutex};
      if (auto iterator = shared_state.iokit_iterators.find(remote_object);
          iterator != shared_state.iokit_iterators.end() &&
          !iterator->second.empty()) {
        object_port = iterator->second.front();
        iterator->second.pop_front();
        object_name =
            copyout_send_locked(shared_state, process.pid, object_port);
      }
    }
    output.write("[iokit] iterator-next pid=" + std::to_string(process.pid) +
                 " iterator-object=" + std::to_string(remote_object) +
                 " service-object=" + std::to_string(object_port) + "\n");
    return write_port_reply(memory, message_address, local_port, message_id,
                            object_name);
  }

  if (message_id == device_mig::id(device_mig::Routine::io_object_get_class) ||
      message_id ==
          device_mig::id(device_mig::Routine::io_registry_entry_get_name)) {
    std::string name;
    {
      std::lock_guard mach_lock{shared_state.mach_mutex};
      if (const auto service = shared_state.iokit_services.find(remote_object);
          service != shared_state.iokit_services.end()) {
        name = service->second.class_name;
      }
    }
    if (name.empty())
      return mach_rcv_invalid_data;
    name.push_back('\0');
    const auto &argument =
        message_id == device_mig::id(device_mig::Routine::io_object_get_class)
            ? object_class_name
            : registry_entry_name;
    const auto name_size = static_cast<std::uint32_t>(name.size());
    const auto reply_size = static_cast<std::uint32_t>(
        argument.reply_offset + ((name_size + sizeof(std::uint32_t) - 1U) &
                                 ~(sizeof(std::uint32_t) - 1U)));
    if (receive_size < reply_size)
      return mach_rcv_invalid_data;
    const auto type_word =
        8U | (8U << 8U) |
        (static_cast<std::uint32_t>(argument.wire_size) << 16U) | (1U << 28U);
    const std::array<std::uint32_t, 11> reply{
        mach_reply_bits,
        reply_size,
        local_port,
        0,
        0,
        message_id + mig_reply_id_delta,
        mach_ndr_native,
        mach_ndr_little_endian,
        iokit_abi::success,
        type_word,
        name_size,
    };
    if (write_reply(memory, message_address, reply) != 0 ||
        !memory.copy_in(message_address + argument.reply_offset,
                        std::span<const std::byte>{
                            reinterpret_cast<const std::byte *>(name.data()),
                            name.size()})) {
      return mach_rcv_invalid_data;
    }
    return 0;
  }

  if (message_id ==
      device_mig::id(device_mig::Routine::io_object_conforms_to)) {
    constexpr const auto &class_argument =
        device_mig::io_object_conforms_to_arguments[1];
    constexpr std::uint32_t reply_size = 40U;
    if (receive_size < reply_size)
      return mach_rcv_invalid_data;
    const auto class_count =
        memory.read32(message_address + class_argument.request_count_offset)
            .value_or(0);
    const auto class_bytes =
        class_count != 0 && class_count <= class_argument.wire_size &&
                class_argument.request_offset + class_count <= send_size
            ? memory.read_bytes(message_address + class_argument.request_offset,
                                class_count)
            : std::nullopt;
    if (!class_bytes)
      return mach_rcv_invalid_data;
    std::string class_name;
    class_name.reserve(class_bytes->size());
    for (const auto byte : *class_bytes)
      class_name.push_back(
          static_cast<char>(std::to_integer<unsigned char>(byte)));
    if (!class_name.empty() && class_name.back() == '\0')
      class_name.pop_back();

    bool conforms = false;
    {
      std::lock_guard mach_lock{shared_state.mach_mutex};
      if (const auto service = shared_state.iokit_services.find(remote_object);
          service != shared_state.iokit_services.end()) {
        conforms = service->second.class_name == class_name ||
                   std::find(service->second.conforms_to.begin(),
                             service->second.conforms_to.end(),
                             class_name) != service->second.conforms_to.end();
      }
    }
    const std::array<std::uint32_t, reply_size / sizeof(std::uint32_t)> reply{
        mach_reply_bits,
        reply_size,
        local_port,
        0,
        0,
        message_id + mig_reply_id_delta,
        mach_ndr_native,
        mach_ndr_little_endian,
        iokit_abi::success,
        conforms ? 1U : 0U,
    };
    output.write("[iokit] conforms pid=" + std::to_string(process.pid) +
                 " service-object=" + std::to_string(remote_object) +
                 " class=" + class_name +
                 " result=" + std::to_string(conforms) + "\n");
    return write_reply(memory, message_address, reply);
  }

  if (message_id ==
      static_cast<std::uint32_t>(iokit_abi::Message::RegistryEntryFromPath)) {
    if (receive_size < 40)
      return mach_rcv_invalid_data;
    const auto path_count =
        memory
            .read32(message_address +
                    registry_entry_from_path.request_count_offset)
            .value_or(0);
    const auto path_bytes =
        path_count != 0 && path_count <= registry_entry_from_path.wire_size &&
                registry_entry_from_path.request_offset + path_count <=
                    send_size
            ? memory.read_bytes(message_address +
                                    registry_entry_from_path.request_offset,
                                path_count)
            : std::nullopt;
    if (!path_bytes)
      return mach_rcv_invalid_data;
    std::string path;
    path.reserve(path_bytes->size());
    for (const auto byte : *path_bytes)
      path.push_back(static_cast<char>(std::to_integer<unsigned char>(byte)));
    if (!path.empty() && path.back() == '\0')
      path.pop_back();

    std::uint32_t entry_object = 0;
    std::uint32_t entry_name = 0;
    {
      std::lock_guard mach_lock{shared_state.mach_mutex};
      const auto service = std::find_if(
          shared_state.iokit_services.begin(),
          shared_state.iokit_services.end(), [&path](const auto &candidate) {
            return candidate.second.registry_path == path;
          });
      entry_object =
          service != shared_state.iokit_services.end()
              ? service->first
              : resolve_task_name_locked(shared_state, process.pid,
                                         process.io_registry_options_port);
      entry_name = copyout_send_locked(shared_state, process.pid, entry_object);
    }
    return write_port_reply(memory, message_address, local_port, message_id,
                            entry_name);
  }

  if (message_id ==
      device_mig::id(device_mig::Routine::io_registry_entry_get_path)) {
    std::string path;
    {
      std::lock_guard mach_lock{shared_state.mach_mutex};
      if (const auto service = shared_state.iokit_services.find(remote_object);
          service != shared_state.iokit_services.end()) {
        path = service->second.registry_path;
      }
    }
    if (path.empty())
      return mach_rcv_invalid_data;
    path.push_back('\0');
    const auto path_size = static_cast<std::uint32_t>(path.size());
    const auto reply_size =
        static_cast<std::uint32_t>(registry_entry_path.reply_offset +
                                   ((path_size + sizeof(std::uint32_t) - 1U) &
                                    ~(sizeof(std::uint32_t) - 1U)));
    if (receive_size < reply_size)
      return mach_rcv_invalid_data;
    const std::array<std::uint32_t, 11> reply{
        mach_reply_bits,
        reply_size,
        local_port,
        0,
        0,
        message_id + mig_reply_id_delta,
        mach_ndr_native,
        mach_ndr_little_endian,
        iokit_abi::success,
        8U | (8U << 8U) |
            (static_cast<std::uint32_t>(registry_entry_path.wire_size) << 16U) |
            (1U << 28U),
        path_size,
    };
    if (write_reply(memory, message_address, reply) != 0 ||
        !memory.copy_in(message_address + registry_entry_path.reply_offset,
                        std::span<const std::byte>{
                            reinterpret_cast<const std::byte *>(path.data()),
                            path.size()})) {
      return mach_rcv_invalid_data;
    }
    output.write("[iokit] path pid=" + std::to_string(process.pid) +
                 " service-object=" + std::to_string(remote_object) +
                 " path=" + path.substr(0, path.size() - 1U) + "\n");
    return 0;
  }

  if (message_id ==
          device_mig::id(
              device_mig::Routine::io_registry_entry_get_parent_iterator) ||
      message_id ==
          device_mig::id(
              device_mig::Routine::io_registry_entry_get_child_iterator)) {
    if (receive_size < 40U)
      return mach_rcv_invalid_data;
    std::uint32_t iterator_object = 0;
    std::uint32_t iterator_name = 0;
    {
      std::lock_guard mach_lock{shared_state.mach_mutex};
      iterator_object = shared_state.allocate_mach_object();
      static_cast<void>(shared_state.mach_port_objects.create(iterator_object));
      auto [iterator, inserted] =
          shared_state.iokit_iterators.try_emplace(iterator_object);
      static_cast<void>(inserted);
      if (message_id ==
          device_mig::id(
              device_mig::Routine::io_registry_entry_get_parent_iterator)) {
        if (const auto service =
                shared_state.iokit_services.find(remote_object);
            service != shared_state.iokit_services.end() &&
            service->second.parent_object != 0) {
          iterator->second.push_back(service->second.parent_object);
        }
      } else {
        for (const auto &[object, service] : shared_state.iokit_services) {
          if (service.parent_object == remote_object)
            iterator->second.push_back(object);
        }
      }
      iterator_name =
          copyout_send_locked(shared_state, process.pid, iterator_object);
    }
    return write_port_reply(memory, message_address, local_port, message_id,
                            iterator_name);
  }

  if (message_id ==
      device_mig::id(device_mig::Routine::io_registry_entry_get_properties)) {
    constexpr std::uint32_t property_reply_size = 52U;
    if (receive_size < property_reply_size)
      return mach_rcv_invalid_data;

    std::optional<std::vector<std::byte>> serialized;
    std::size_t property_count = 0;
    {
      std::lock_guard mach_lock{shared_state.mach_mutex};
      if (shared_state.iokit_registry_root_object == remote_object) {
        auto properties = registry_root_properties(shared_state);
        if (const auto service =
                shared_state.iokit_services.find(remote_object);
            service != shared_state.iokit_services.end()) {
          properties.insert(service->second.properties.begin(),
                            service->second.properties.end());
        }
        serialized = serialize_properties(properties);
        property_count = properties.size();
      } else if (const auto service =
                     shared_state.iokit_services.find(remote_object);
                 service != shared_state.iokit_services.end()) {
        serialized = serialize_properties(service->second.properties);
        property_count = service->second.properties.size();
      }
    }
    if (!serialized)
      return mach_rcv_invalid_data;

    const auto serialized_size = static_cast<std::uint32_t>(serialized->size());
    const auto mapped_size = (serialized_size + AddressSpace::page_size - 1U) &
                             ~(AddressSpace::page_size - 1U);
    const auto region = mach_support::find_free_guest_region(
        memory, mach_support::ool_results_base, mapped_size);
    if (!region ||
        !memory.map(*region, mapped_size,
                    MemoryPermission::Read | MemoryPermission::Write) ||
        !memory.copy_in(*region, *serialized)) {
      if (region)
        static_cast<void>(memory.unmap(*region, mapped_size));
      return mach_rcv_invalid_data;
    }
    const std::array<std::uint32_t, property_reply_size / sizeof(std::uint32_t)>
        reply{darwin::mig_wire::message_bits(
                  darwin::mig_wire::disposition_move_send_once, 0, true),
              property_reply_size,
              local_port,
              0,
              0,
              message_id + mig_reply_id_delta,
              1,
              *region,
              serialized_size,
              darwin::mig_wire::ool_descriptor_metadata(
                  false, darwin::mig_wire::ool_copy_physical),
              mach_ndr_native,
              mach_ndr_little_endian,
              serialized_size};
    const auto result = write_reply(memory, message_address, reply);
    if (result != 0)
      static_cast<void>(memory.unmap(*region, mapped_size));
    output.write("[iokit] properties pid=" + std::to_string(process.pid) +
                 " service-object=" + std::to_string(remote_object) +
                 " count=" + std::to_string(property_count) + "\n");
    return result;
  }

  if (message_id == static_cast<std::uint32_t>(
                        iokit_abi::Message::RegistryEntryGetProperty) ||
      message_id ==
          device_mig::id(device_mig::Routine::
                             io_registry_entry_get_property_recursively)) {
    constexpr std::uint32_t simple_reply_size = 36U;
    constexpr std::uint32_t property_reply_size =
        registry_property_value.reply_count_offset +
        darwin::mig_wire::word_size;
    if (receive_size < simple_reply_size)
      return mach_rcv_invalid_data;

    std::optional<std::vector<std::byte>> name_bytes;
    if (message_id == static_cast<std::uint32_t>(
                          iokit_abi::Message::RegistryEntryGetProperty)) {
      const auto name_count =
          memory
              .read32(message_address +
                      registry_property_name.request_count_offset)
              .value_or(0);
      if (name_count != 0 && name_count <= registry_property_name.wire_size &&
          registry_property_name.request_offset + name_count <= send_size) {
        name_bytes = memory.read_bytes(
            message_address + registry_property_name.request_offset,
            name_count);
      }
    } else {
      std::array<std::uint32_t, recursive_registry_property.size()>
          element_counts{};
      element_counts[1] =
          memory
              .read32(message_address +
                      recursive_registry_property[1].request_count_offset)
              .value_or(0);
      const auto plane_layout = xnu792::mig::compute_wire_layout(
          recursive_registry_property, xnu792::mig::WireLayoutSide::Request,
          element_counts);
      if (!plane_layout)
        return mach_rcv_invalid_data;
      element_counts[2] =
          memory.read32(message_address + (*plane_layout)[2].count_offset)
              .value_or(0);
      const auto layout = xnu792::mig::compute_wire_layout(
          recursive_registry_property, xnu792::mig::WireLayoutSide::Request,
          element_counts);
      if (!layout || element_counts[1] == 0 ||
          element_counts[1] > recursive_registry_property[1].wire_size ||
          element_counts[2] == 0 ||
          element_counts[2] > recursive_registry_property[2].wire_size ||
          (*layout)[3].offset + recursive_registry_property[3].wire_size >
              send_size) {
        return mach_rcv_invalid_data;
      }
      name_bytes = memory.read_bytes(message_address + (*layout)[2].offset,
                                     element_counts[2]);
    }
    if (!name_bytes)
      return mach_rcv_invalid_data;

    std::string property_name;
    property_name.reserve(name_bytes->size());
    for (const auto byte : *name_bytes)
      property_name.push_back(
          static_cast<char>(std::to_integer<unsigned char>(byte)));
    if (!property_name.empty() && property_name.back() == '\0')
      property_name.pop_back();

    std::optional<KernelSharedState::IOKitRegistryProperty> property;
    {
      std::lock_guard mach_lock{shared_state.mach_mutex};
      if (shared_state.iokit_registry_root_object == remote_object) {
        const auto properties = registry_root_properties(shared_state);
        const auto value = properties.find(property_name);
        if (value != properties.end())
          property = value->second;
      }
      if (!property) {
        const auto service = shared_state.iokit_services.find(remote_object);
        if (service != shared_state.iokit_services.end()) {
          const auto value = service->second.properties.find(property_name);
          if (value != service->second.properties.end())
            property = value->second;
        }
      }
    }
    if (!property) {
      const std::array<std::uint32_t, simple_reply_size / sizeof(std::uint32_t)>
          reply{darwin::mig_wire::message_bits(
                    darwin::mig_wire::disposition_move_send_once),
                simple_reply_size,
                local_port,
                0,
                0,
                message_id + mig_reply_id_delta,
                mach_ndr_native,
                mach_ndr_little_endian,
                iokit_abi::not_found};
      output.write("[iokit] property pid=" + std::to_string(process.pid) +
                   " service-object=" + std::to_string(remote_object) +
                   " name=" + property_name + " result=not-found\n");
      return write_reply(memory, message_address, reply);
    }

    if (receive_size < property_reply_size)
      return mach_rcv_invalid_data;
    const auto serialized = serialize_property(*property);
    const auto serialized_size = static_cast<std::uint32_t>(serialized.size());
    const auto mapped_size = (serialized_size + AddressSpace::page_size - 1U) &
                             ~(AddressSpace::page_size - 1U);
    const auto region = mach_support::find_free_guest_region(
        memory, mach_support::ool_results_base, mapped_size);
    if (!region ||
        !memory.map(*region, mapped_size,
                    MemoryPermission::Read | MemoryPermission::Write) ||
        !memory.copy_in(*region, serialized)) {
      if (region)
        static_cast<void>(memory.unmap(*region, mapped_size));
      return mach_rcv_invalid_data;
    }

    const std::array<std::uint32_t, property_reply_size / sizeof(std::uint32_t)>
        reply{darwin::mig_wire::message_bits(
                  darwin::mig_wire::disposition_move_send_once, 0, true),
              property_reply_size,
              local_port,
              0,
              0,
              message_id + mig_reply_id_delta,
              1,
              *region,
              serialized_size,
              darwin::mig_wire::ool_descriptor_metadata(
                  false, darwin::mig_wire::ool_copy_physical),
              mach_ndr_native,
              mach_ndr_little_endian,
              serialized_size};
    const auto result = write_reply(memory, message_address, reply);
    if (result != 0)
      static_cast<void>(memory.unmap(*region, mapped_size));
    output.write("[iokit] property pid=" + std::to_string(process.pid) +
                 " service-object=" + std::to_string(remote_object) +
                 " name=" + property_name + " bytes=" +
                 std::to_string(property->value.size()) + " result=success\n");
    return result;
  }

  if (message_id == static_cast<std::uint32_t>(
                        iokit_abi::Message::RegistryEntrySetProperties) ||
      message_id ==
          device_mig::id(device_mig::Routine::io_connect_set_properties)) {
    if (receive_size < 40U)
      return mach_rcv_invalid_data;
    const auto descriptor_count =
        memory
            .read32(message_address +
                    darwin::mig_wire::complex_descriptor_count_offset)
            .value_or(0);
    const auto data_address =
        memory.read32(message_address + set_properties_data.request_offset)
            .value_or(0);
    const auto data_size =
        memory
            .read32(message_address + set_properties_data.request_offset +
                    darwin::mig_wire::word_size)
            .value_or(0);
    const auto data = descriptor_count == 1 && data_size <= 64U * 1024U
                          ? memory.read_bytes(data_address, data_size)
                          : std::nullopt;
    bool network_stack_connection = false;
    if (message_id ==
        device_mig::id(device_mig::Routine::io_connect_set_properties)) {
      std::lock_guard mach_lock{shared_state.mach_mutex};
      const auto connection =
          shared_state.iokit_connections.find(remote_object);
      if (connection != shared_state.iokit_connections.end()) {
        const auto service =
            shared_state.iokit_services.find(connection->second.service_port);
        network_stack_connection =
            service != shared_state.iokit_services.end() &&
            service->second.class_name == io_network_stack_class;
      }
    }
    const auto accepted =
        data &&
        (message_id !=
             device_mig::id(device_mig::Routine::io_connect_set_properties) ||
         network_stack_connection);
    const auto result = accepted ? iokit_abi::success : iokit_abi::bad_argument;
    if (data &&
        message_id == static_cast<std::uint32_t>(
                          iokit_abi::Message::RegistryEntrySetProperties))
      shared_state.nvram_serialized = *data;
    const std::array<std::uint32_t, 10> reply{
        18,          40,          local_port, 0,      0, message_id + 100,
        0x00000000U, 0x00000001U, 0,          result,
    };
    return write_reply(memory, message_address, reply);
  }

  if (message_id ==
      static_cast<std::uint32_t>(iokit_abi::Message::ServiceGetBusyState)) {
    // IOKitGetBusyState queries the master service plane as well as individual
    // IOService objects. The modeled tree has no asynchronous service-start
    // work, so its XNU getBusyState value is consistently quiet.
    constexpr std::uint32_t reply_size = 40U;
    if (receive_size < reply_size)
      return mach_rcv_invalid_data;
    const std::array<std::uint32_t, reply_size / sizeof(std::uint32_t)> reply{
        mach_reply_bits,
        reply_size,
        local_port,
        0,
        0,
        message_id + mig_reply_id_delta,
        mach_ndr_native,
        mach_ndr_little_endian,
        iokit_abi::success,
        iokit_abi::service_busy_state_quiet,
    };
    output.write("[iokit] busy-state pid=" + std::to_string(process.pid) +
                 " service-object=" + std::to_string(remote_object) +
                 " state=quiet\n");
    return write_reply(memory, message_address, reply);
  }

  if (message_id ==
      static_cast<std::uint32_t>(iokit_abi::Message::ServiceOpen)) {
    if (receive_size < 40U)
      return mach_rcv_invalid_data;
    // The task port is one complex descriptor and connect_type follows
    // its NDR record, exactly as emitted by the target firmware stub.
    const auto descriptor_count =
        memory
            .read32(message_address +
                    darwin::mig_wire::complex_descriptor_count_offset)
            .value_or(0);
    if (descriptor_count != 1)
      return mach_rcv_invalid_data;
    const auto owning_task_name =
        memory.read32(message_address + service_open_owning_task.request_offset)
            .value_or(0);
    if (owning_task_name == xnu792::ipc::null_name) {
      return mach_rcv_invalid_data;
    }
    const auto connect_type =
        memory
            .read32(message_address + service_open_connect_type.request_offset)
            .value_or(0);
    std::uint32_t connection_object = 0;
    std::uint32_t connection_name = 0;
    {
      std::lock_guard mach_lock{shared_state.mach_mutex};
      const auto owning_task =
          resolve_task_name_locked(shared_state, process.pid, owning_task_name);
      const auto owning_pid = shared_state.task_port_pids.find(owning_task);
      if (owning_pid == shared_state.task_port_pids.end() ||
          owning_pid->second != process.pid) {
        return mach_rcv_invalid_data;
      }
      if (!shared_state.iokit_services.contains(remote_object)) {
        return write_port_reply(memory, message_address, local_port, message_id,
                                0);
      }
      connection_object = shared_state.allocate_mach_object();
      static_cast<void>(
          shared_state.mach_port_objects.create(connection_object));
      shared_state.mach_queues.try_emplace(connection_object);
      shared_state.iokit_connections.emplace(
          connection_object, KernelSharedState::IOKitConnection{
                                 remote_object, process.pid, connect_type});
      connection_name =
          copyout_send_locked(shared_state, process.pid, connection_object);
    }
    output.write("[iokit] open pid=" + std::to_string(process.pid) +
                 " service-object=" + std::to_string(remote_object) +
                 " connection-name=" + std::to_string(connection_name) +
                 " connection-object=" + std::to_string(connection_object) +
                 " type=" + std::to_string(connect_type) + "\n");
    return write_port_reply(memory, message_address, local_port, message_id,
                            connection_name);
  }

  if (message_id ==
      static_cast<std::uint32_t>(iokit_abi::Message::ServiceOpenExtended)) {
    using namespace iokit_abi::service_open_extended;
    if (receive_size < reply_size)
      return mach_rcv_invalid_data;
    const auto descriptor_count =
        memory
            .read32(message_address +
                    darwin::mig_wire::complex_descriptor_count_offset)
            .value_or(0);
    if (descriptor_count != request_descriptor_count) {
      return mach_rcv_invalid_data;
    }
    const auto requested_connect_type =
        send_size >= connect_type_offset + connect_type_size
            ? memory.read32(message_address + connect_type_offset).value_or(0)
            : 0U;
    std::uint32_t connection_object = 0;
    std::uint32_t connection_name = 0;
    bool supported_service = false;
    KernelSharedState::IOKitUserClientProfile profile{
        KernelSharedState::IOKitUserClientProfile::None};
    {
      std::lock_guard mach_lock{shared_state.mach_mutex};
      const auto service = shared_state.iokit_services.find(remote_object);
      if (service != shared_state.iokit_services.end()) {
        profile = service->second.user_client_profile;
        supported_service =
            profile != KernelSharedState::IOKitUserClientProfile::None;
      }
      if (supported_service) {
        connection_object = shared_state.allocate_mach_object();
        static_cast<void>(
            shared_state.mach_port_objects.create(connection_object));
        shared_state.mach_queues.try_emplace(connection_object);
        shared_state.iokit_connections.emplace(
            connection_object,
            KernelSharedState::IOKitConnection{
                remote_object, process.pid,
                connection_type_for_profile(profile, requested_connect_type)});
        connection_name =
            copyout_send_locked(shared_state, process.pid, connection_object);
      }
    }
    const auto result =
        supported_service ? iokit_abi::success : iokit_abi::unsupported;
    const std::array<std::uint32_t, reply_word_count> reply{
        0x80000012U,      reply_size,  local_port,      0, 0,
        message_id + 100, 1,           connection_name, 0, 0x00110000U,
        0x00000000U,      0x00000001U, result,
    };
    output.write("[iokit] open-extended pid=" + std::to_string(process.pid) +
                 " service-object=" + std::to_string(remote_object) +
                 " connection-name=" + std::to_string(connection_name) +
                 " connection-object=" + std::to_string(connection_object) +
                 " profile=" + std::string{user_client_profile_name(profile)} +
                 " type=" + std::to_string(requested_connect_type) +
                 " result=" + (supported_service ? "success" : "unsupported") +
                 "\n");
    return write_reply(memory, message_address, reply);
  }

  if (message_id ==
      static_cast<std::uint32_t>(iokit_abi::Message::ConnectMethod)) {
    const auto request = read_connect_method_request(memory, message_address,
                                                     send_size, receive_size);
    if (!request)
      return mach_rcv_invalid_data;
    const auto display_result = kernel_iokit::display::dispatch_connect_method(
        shared_state, process, remote_object, request->selector,
        std::span<const std::uint64_t>{request->scalar_input.data(),
                                       request->scalar_input_count},
                                       request->inband_input, request->scalar_output_capacity);
    const auto baseband_result =
        display_result
            ? std::optional<kernel_iokit::baseband::MethodResult>{}
            : kernel_iokit::baseband::dispatch_connect_method(
                  shared_state, process, remote_object, request->selector,
                  std::span<const std::uint64_t>{request->scalar_input.data(),
                                                 request->scalar_input_count},
                  request->inband_input, request->scalar_output_capacity);
    const auto audio_result =
        display_result || baseband_result
            ? std::optional<kernel_iokit::audio::MethodResult>{}
            : kernel_iokit::audio::dispatch_connect_method(
                  shared_state, process, remote_object, request->selector,
                  std::span<const std::uint64_t>{request->scalar_input.data(),
                                                 request->scalar_input_count},
                  request->inband_input, request->scalar_output_capacity);
    const auto camera_result =
        display_result || baseband_result || audio_result
            ? std::optional<kernel_iokit::camera::MethodResult>{}
            : kernel_iokit::camera::dispatch_connect_method(
                  shared_state, process, memory, surfaces, remote_object,
                  request->selector,
                  std::span<const std::uint64_t>{request->scalar_input.data(),
                                                 request->scalar_input_count},
                  request->inband_input, request->scalar_output_capacity);
    const auto jpeg_result =
        display_result || baseband_result || audio_result || camera_result
            ? std::optional<kernel_iokit::jpeg::MethodResult>{}
            : kernel_iokit::jpeg::dispatch_connect_method(
                  shared_state, process, memory, surfaces, remote_object,
                  request->selector,
                  std::span<const std::uint64_t>{request->scalar_input.data(),
                                                 request->scalar_input_count},
                  request->inband_input, request->scalar_output_capacity,
                  request->inband_output_capacity);
    const auto mbx_result =
        display_result || baseband_result || audio_result || camera_result ||
                jpeg_result
            ? std::optional<kernel_iokit::mbx::MethodResult>{}
            : kernel_iokit::mbx::dispatch_connect_method(
                  memory, shared_state, process, remote_object,
                  request->selector,
                  std::span<const std::uint64_t>{request->scalar_input.data(),
                                                 request->scalar_input_count},
                  request->inband_input, request->scalar_output_capacity,
                  request->inband_output_capacity);
    const auto graphics_result =
        display_result || baseband_result || audio_result || camera_result ||
                jpeg_result || mbx_result
            ? std::optional<kernel_iokit::graphics::MethodResult>{}
            : kernel_iokit::graphics::dispatch_connect_method(
                  memory, shared_state, process, remote_object,
                  request->selector,
                  std::span<const std::uint64_t>{request->scalar_input.data(),
                                                 request->scalar_input_count},
                  request->inband_input, request->scalar_output_capacity,
                  request->inband_output_capacity);
    const auto mobile_file_integrity_result =
        display_result || baseband_result || audio_result || camera_result ||
                jpeg_result || mbx_result || graphics_result
            ? std::optional<
                  kernel_iokit::mobile_file_integrity::MethodResult>{}
            : kernel_iokit::mobile_file_integrity::dispatch_connect_method(
                  shared_state, process, remote_object, request->selector,
                  std::span<const std::uint64_t>{request->scalar_input.data(),
                                                 request->scalar_input_count},
                  request->inband_input, request->scalar_output_capacity,
                  request->inband_output_capacity);
    const ConnectMethodResult result =
        display_result
            ? ConnectMethodResult{display_result->return_code,
                                  std::move(display_result->scalar_output),
                                  {}}
        : audio_result
            ? ConnectMethodResult{audio_result->return_code,
                                  std::move(audio_result->scalar_output),
                                  {}}
        : baseband_result
            ? ConnectMethodResult{baseband_result->return_code,
                                  std::move(baseband_result->scalar_output),
                                  {}}
        : camera_result
            ? ConnectMethodResult{camera_result->return_code,
                                  std::move(camera_result->scalar_output),
                                  {}}
        : jpeg_result
            ? ConnectMethodResult{jpeg_result->return_code, {},
                                  std::move(jpeg_result->inband_output)}
        : mbx_result
            ? ConnectMethodResult{mbx_result->return_code,
                                  std::move(mbx_result->scalar_output),
                                  std::move(mbx_result->inband_output)}
        : graphics_result
            ? ConnectMethodResult{graphics_result->return_code,
                                  std::move(graphics_result->scalar_output),
                                  std::move(graphics_result->inband_output)}
        : mobile_file_integrity_result
            ? ConnectMethodResult{
                  mobile_file_integrity_result->return_code,
                  {},
                  std::move(mobile_file_integrity_result->inband_output)}
            : ConnectMethodResult{};
    std::uint64_t method_call_count = 0;
    bool vsync_enabled = false;
    if (iokit_abi::is_apple_h1clcd_vsync_selector(request->selector)) {
      std::lock_guard mach_lock{shared_state.mach_mutex};
      if (const auto registration =
              shared_state.iokit_display_vsync.find(remote_object);
          registration != shared_state.iokit_display_vsync.end()) {
        method_call_count = registration->second.method_call_count;
        vsync_enabled = registration->second.enabled;
      }
    }
    const auto log_method =
        method_call_count == 0 || trace_repeated_call(method_call_count);
    if (log_method) {
      output.write(
          "[iokit] method pid=" + std::to_string(process.pid) +
          " connection-name=" + std::to_string(remote_port) +
          " connection-object=" + std::to_string(remote_object) +
          " selector=" + std::to_string(request->selector) +
          " scalar-in=" + std::to_string(request->scalar_input_count) +
          (request->scalar_input_count >= 1U
               ? " scalar0=" + std::to_string(request->scalar_input[0])
               : "") +
          (request->scalar_input_count >= 2U
               ? " scalar1=" + std::to_string(request->scalar_input[1])
               : "") +
          " scalar-out=" + std::to_string(result.scalar_output.size()) +
          " result=" + std::to_string(result.return_code) +
          (method_call_count == 0
               ? std::string{}
               : " call=" + std::to_string(method_call_count) +
                     " enabled=" + std::to_string(vsync_enabled)) +
          (method_call_count == 0 ? std::string{}
                                  : format_call_site(memory, call_site)) +
          "\n");
    }
    return write_connect_method_reply(memory, message_address, local_port,
                                      message_id, receive_size, result);
  }

  if (message_id ==
      static_cast<std::uint32_t>(iokit_abi::Message::ServiceClose)) {
    if (receive_size < 24)
      return mach_rcv_invalid_data;
    kernel_iokit::audio::close_connection(memory, shared_state, remote_object);
    kernel_iokit::graphics::close_connection(memory, shared_state,
                                              remote_object);
    kernel_iokit::mbx::close_connection(memory, shared_state, remote_object);
    {
      std::lock_guard mach_lock{shared_state.mach_mutex};
      kernel_iokit::camera::close_connection_locked(shared_state,
                                                    remote_object);
      // ServiceClose is an ipc_port teardown, not just an IOKit map erase.
      // Convert foreign Send rights to dead names and discard queued
      // transfers through the common lifecycle path; the old direct erase
      // left stale connection/notification state and could resurrect a queue
      // when a delayed reply was copied out.
      static_cast<void>(
          shared_state.mach_namespaces.mark_object_dead(remote_object));
      mach_support::remove_port_object_locked(shared_state, remote_object);
      static_cast<void>(
          shared_state.mach_namespaces.deallocate(process.pid, remote_port));
    }
    const std::array<std::uint32_t, 9> reply{
        18,          36,          local_port,         0, 0, message_id + 100,
        0x00000000U, 0x00000001U, iokit_abi::success,
    };
    return write_reply(memory, message_address, reply);
  }

  output.write("[iokit] unhandled pid=" + std::to_string(process.pid) +
               " id=" + std::to_string(message_id) +
               " remote-object=" + std::to_string(remote_object) + "\n");
  return std::nullopt;
}

} // namespace ilemu
