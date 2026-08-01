#include "ilemu/core_media_hle.hpp"

#include <cstdint>
#include <string>
#include <string_view>

#include "ilemu/userland_hle.hpp"

namespace ilemu {
namespace {

constexpr std::string_view core_media_image{
    "/System/Library/PrivateFrameworks/CoreMedia.framework/CoreMedia"};
constexpr std::uint32_t parameter_error = 0xffffffceU; // -50

} // namespace

CoreMediaHle::CoreMediaHle(UserlandHleRegistry &registry) {
  registry.register_function(
      std::string{core_media_image},
      "_CMSessionMgrCopyDeviceRouteForAudioCategory",
      &CoreMediaHle::copy_device_route_for_audio_category);
}

void CoreMediaHle::copy_device_route_for_audio_category(UserlandHleCall &call) {
  // The Darwin ABI passes two output CFStringRef pointers in r1/r2. A null
  // route pair is the native answer when no physical route exists; the guest
  // MediaToolbox server already treats both outputs as optional.
  const auto route_name = call.argument(1);
  const auto route_identifier = call.argument(2);
  const auto name_written = route_name == 0U || call.write32(route_name, 0U);
  const auto identifier_written =
      route_identifier == 0U || call.write32(route_identifier, 0U);
  call.set_return(name_written && identifier_written ? 0U : parameter_error);
}

} // namespace ilemu
