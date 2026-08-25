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

    constexpr std::string_view lockdown_image { "/usr/lib/liblockdown.dylib" };
    constexpr std::string_view core_foundation_image {
        "/CoreFoundation.framework/CoreFoundation"
    };
    constexpr std::string_view copy_value { "_lockdown_copy_value" };
    constexpr std::string_view activation_state_key {
        "_kLockdownActivationStateKey"
    };
    constexpr std::string_view brick_state_key { "_kLockdownBrickStateKey" };
    constexpr std::string_view create_cf_string {
        "_CFStringCreateWithCString"
    };
    constexpr std::string_view cf_boolean_false { "_kCFBooleanFalse" };
    constexpr std::string_view cf_boolean_true { "_kCFBooleanTrue" };
    constexpr std::uint32_t cf_string_encoding_utf8 { 0x08000100U };

    bool is_lockdown_query(UserlandHleCall& call, std::string_view key_symbol,
        std::string_view key_name)
    {
        if (const auto key_variable = call.symbol_address(key_symbol)) {
            if (const auto key_object = call.memory().read32(*key_variable);
                key_object && *key_object == call.argument(2)) {
                return true;
            }
        }
        return call.objc_string_argument(2) ==
               std::optional<std::string> { key_name };
    }

    std::uint32_t exported_object(
        UserlandHleCall& call, std::string_view symbol)
    {
        const auto address = call.symbol_address(symbol);
        return address ? call.memory().read32(*address).value_or(0) : 0;
    }

} // namespace

void register_lockdown_hle(UserlandHleRegistry& registry,
    std::optional<bool> activated, LockdownFirmwareProfile profile)
{
    if (!activated)
        return;

    registry.register_guest_function(std::string { core_foundation_image },
        std::string { create_cf_string });
    if (profile.brick_state) {
        registry.register_guest_data_symbol(
            std::string { core_foundation_image },
            std::string { cf_boolean_false });
        registry.register_guest_data_symbol(
            std::string { core_foundation_image },
            std::string { cf_boolean_true });
    }
    registry.register_function(std::string { lockdown_image },
        std::string { copy_value },
        [activated = *activated, profile](UserlandHleCall& call) {
            if (profile.brick_state &&
                is_lockdown_query(call, brick_state_key, "BrickState")) {
                const auto value = exported_object(
                    call, activated ? cf_boolean_false : cf_boolean_true);
                if (value == 0) {
                    call.resume_original_persistently();
                } else {
                    call.set_return(value);
                }
                return;
            }
            if (!is_lockdown_query(
                    call, activation_state_key, "ActivationState")) {
                // A process can query unrelated Lockdown keys before the
                // activation and brick state. Keep this boundary installed for
                // later queries.
                call.resume_original_persistently();
                return;
            }
            if (!call.symbol_address(create_cf_string)) {
                call.resume_original_persistently();
                return;
            }

            const auto value =
                call.intern_string(activated ? "Activated" : "Unactivated");
            if (value == 0) {
                call.set_return(0);
                return;
            }

            auto& registers = call.cpu().registers();
            registers[0] = 0;
            registers[1] = value;
            registers[2] = cf_string_encoding_utf8;
            if (!call.call_guest_function(
                    create_cf_string, [](UserlandHleCall&) { })) {
                call.set_return(0);
            }
        });
}

} // namespace ilemu
