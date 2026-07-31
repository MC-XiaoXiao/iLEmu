#include "ilemu/kernel_iokit_baseband.hpp"

#include "ilemu/kernel_shared_state.hpp"

#include <algorithm>
#include <string>

namespace ilemu::kernel_iokit::baseband {

namespace {

bool contains(std::span<const std::byte> matching, std::string_view value) {
  return std::search(matching.begin(), matching.end(), value.begin(),
                     value.end(), [](std::byte byte, char character) {
                       return std::to_integer<unsigned char>(byte) ==
                              static_cast<unsigned char>(character);
                     }) != matching.end();
}

} // namespace

bool matches_service(std::span<const std::byte> matching) {
  return contains(matching, service_class) || contains(matching, registry_name);
}

std::uint32_t ensure_service_locked(KernelSharedState &state) {
  if (state.baseband_service != 0)
    return state.baseband_service;

  const auto object = state.allocate_mach_object();
  state.baseband_service = object;
  static_cast<void>(state.mach_port_objects.create(object));
  state.mach_queues.try_emplace(object);
  state.iokit_services.emplace(
      object,
      KernelSharedState::IOKitService{
          std::string{service_class},
          {"IOService"},
          {},
          {},
          0,
          KernelSharedState::IOKitUserClientProfile::Generic});
  return object;
}

} // namespace ilemu::kernel_iokit::baseband
