#pragma once

#include <cstdint>

namespace ilemu::iokit_abi {

// Darwin 8 osfmk/device/device.defs subsystem iokit 2800. Later routines used
// by the iPhoneOS 1.0 IOKit build retain their firmware-observed IDs.
enum class Message : std::uint32_t {
  IteratorNext = 2802,
  ServiceGetMatchingServices = 2804,
  RegistryEntryGetProperty = 2805,
  RegistryEntryFromPath = 2809,
  ServiceOpen = 2815,
  ServiceClose = 2816,
  ConnectSetNotificationPort = 2818,
  ConnectMapMemory = 2819,
  RegistryEntrySetProperties = 2828,
  ServiceGetBusyState = 2831,
  ServiceAddNotification = 2849,
  ServiceAddInterestNotification = 2850,
  ServiceOpenExtended = 2862,
  // Private iPhoneOS-era extension retained by the firmware IOKit client.
  // The request ID is loaded by _io_connect_method in the 1.0 IOKit image.
  ConnectMethod = 2865,
};

enum class AppleH1ClcdSelector : std::uint32_t {
  GetLayerDefaultSurface = 3,
  SetVSyncNotificationsV1 = 8,
  SetVSyncNotifications = 9,
  RequestPowerChangeV1 = 13,
  RequestPowerChange = 14,
};

constexpr bool is_apple_h1clcd_vsync_selector(std::uint32_t selector) {
  return selector == static_cast<std::uint32_t>(
                         AppleH1ClcdSelector::SetVSyncNotificationsV1) ||
         selector == static_cast<std::uint32_t>(
                         AppleH1ClcdSelector::SetVSyncNotifications);
}

constexpr bool is_apple_h1clcd_power_selector(std::uint32_t selector) {
  return selector == static_cast<std::uint32_t>(
                         AppleH1ClcdSelector::RequestPowerChangeV1) ||
         selector == static_cast<std::uint32_t>(
                         AppleH1ClcdSelector::RequestPowerChange);
}

inline constexpr std::uint32_t success = 0;
inline constexpr std::uint32_t not_found = 0xe00002f0U;
inline constexpr std::uint32_t bad_argument = 0xe00002c2U;
inline constexpr std::uint32_t unsupported = 0xe00002c7U;

// A zero busy count means the modeled I/O service tree has completed all
// outstanding start/stop transitions.
inline constexpr std::uint32_t service_busy_state_quiet = 0;

inline constexpr std::uint32_t apple_h1clcd_service_type = 0;
inline constexpr std::uint32_t power_root_service_type = 0;
// CoreSurface IDs are transport handles, not guest pointers. Keep the display
// driver's reserved surface outside the IDs normally allocated at process
// startup; CoreSurfaceHle advances its allocator after a lookup of this ID.
inline constexpr std::uint32_t apple_h1clcd_default_surface_id = 0x100U;

namespace display_vsync {

// OSMessageNotification.h from XNU 792: a kernel async completion is Mach
// message 53 with notification type 150 and an eight-natural reference.
inline constexpr std::uint32_t message_identifier = 53;
inline constexpr std::uint32_t async_completion_type = 150;
inline constexpr std::uint32_t async_reference_count = 8;
inline constexpr std::uint32_t async_reserved_index = 0;
inline constexpr std::uint32_t async_callout_index = 1;
inline constexpr std::uint32_t async_refcon_index = 2;
inline constexpr std::uint32_t completion_argument_count = 6;
inline constexpr std::uint32_t refresh_rate_hz = 60;
inline constexpr std::uint64_t period_absolute_time =
    1'000'000'000ULL / refresh_rate_hz;

} // namespace display_vsync

namespace service_open_extended {

// Firmware-private routine 2862. It is absent from XNU 792 device.defs, so
// keep its observed request/reply contract separate from generated MIG data.
inline constexpr std::uint32_t request_descriptor_count = 2;
inline constexpr std::uint32_t reply_size = 52;
inline constexpr std::uint32_t reply_word_count =
    reply_size / sizeof(std::uint32_t);
inline constexpr std::uint32_t result_offset = 48;

} // namespace service_open_extended

namespace service_interest_notification {

inline constexpr std::uint32_t request_descriptor_count = 1;
inline constexpr std::uint32_t reply_size = 40;
inline constexpr std::uint32_t maximum_interest_name_size = 128;
inline constexpr std::uint32_t maximum_reference_count = 8;

} // namespace service_interest_notification

namespace connect_method {

// Firmware-observed ARM32 MIG layout for io_connect_method. Scalar values are
// 64-bit even though the guest is 32-bit. Variable request fields following
// scalar_input and inband_input are compressed by MIG before mach_msg.
inline constexpr std::uint32_t selector_offset = 32;
inline constexpr std::uint32_t scalar_input_count_offset = 36;
inline constexpr std::uint32_t scalar_input_offset = 40;
inline constexpr std::uint32_t maximum_scalar_count = 16;
inline constexpr std::uint32_t maximum_inband_size = 4096;
inline constexpr std::uint32_t scalar_size = sizeof(std::uint64_t);
inline constexpr std::uint32_t inband_count_size = sizeof(std::uint32_t);
inline constexpr std::uint32_t trailing_request_size = 24;
inline constexpr std::uint32_t minimum_request_size =
    scalar_input_offset + inband_count_size + trailing_request_size;

// MIG compresses the reply to the actual scalar and inband counts. The
// generated validator rebases its fixed-array view by scalar_count * 8 - 128,
// so inband output follows the last returned scalar rather than all 16 slots.
inline constexpr std::uint32_t return_code_offset = 32;
inline constexpr std::uint32_t scalar_output_count_offset = 36;
inline constexpr std::uint32_t scalar_output_offset = 40;
constexpr std::uint32_t inband_output_count_offset(std::uint32_t scalar_count) {
  return scalar_output_offset + scalar_count * scalar_size;
}
constexpr std::uint32_t inband_output_offset(std::uint32_t scalar_count) {
  return inband_output_count_offset(scalar_count) + sizeof(std::uint32_t);
}
inline constexpr std::uint32_t minimum_reply_size = inband_output_offset(0);
inline constexpr std::uint32_t maximum_reply_size =
    inband_output_offset(maximum_scalar_count) + maximum_inband_size;
// Literal receive capacity passed by the firmware _io_connect_method stub.
inline constexpr std::uint32_t firmware_receive_buffer_size = 0x10b8U;

} // namespace connect_method

} // namespace ilemu::iokit_abi
