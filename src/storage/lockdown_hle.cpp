#include "ilemu/lockdown_hle.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "ilemu/address_space.hpp"
#include "ilemu/cpu.hpp"
#include "ilemu/userland_hle.hpp"

namespace ilemu {
namespace {

constexpr std::string_view lockdown_image{"/usr/lib/liblockdown.dylib"};
constexpr std::string_view core_foundation_image{
    "/System/Library/Frameworks/CoreFoundation.framework/CoreFoundation"};
constexpr std::string_view copy_value{"_lockdown_copy_value"};
constexpr std::string_view activation_state_key{"_kLockdownActivationStateKey"};
constexpr std::string_view create_cf_string{"_CFStringCreateWithCString"};
constexpr std::uint32_t cf_string_encoding_utf8{0x08000100U};

bool is_activation_state_query(UserlandHleCall &call) {
  if (const auto key_variable = call.symbol_address(activation_state_key)) {
    if (const auto key_object = call.memory().read32(*key_variable);
        key_object && *key_object == call.argument(2)) {
      return true;
    }
  }
  return call.objc_string_argument(2) ==
         std::optional<std::string>{"ActivationState"};
}

} // namespace

void register_lockdown_hle(UserlandHleRegistry &registry,
                           std::optional<bool> activated) {
  if (!activated)
    return;

  registry.register_guest_function(std::string{core_foundation_image},
                                   std::string{create_cf_string});
  registry.register_function(
      std::string{lockdown_image}, std::string{copy_value},
      [activated = *activated](UserlandHleCall &call) {
        if (!is_activation_state_query(call) ||
            !call.symbol_address(create_cf_string)) {
          call.resume_original_persistently();
          return;
        }

        const auto value =
            call.intern_string(activated ? "Activated" : "Unactivated");
        if (value == 0) {
          call.set_return(0);
          return;
        }

        auto &registers = call.cpu().registers();
        registers[0] = 0;
        registers[1] = value;
        registers[2] = cf_string_encoding_utf8;
        if (!call.call_guest_function(create_cf_string,
                                      [](UserlandHleCall &) {})) {
          call.set_return(0);
        }
      });
}

} // namespace ilemu
