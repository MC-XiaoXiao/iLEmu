#include "ilegacysim/core_telephony_hle.hpp"

#include <array>
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>

#include "ilegacysim/address_space.hpp"
#include "ilegacysim/cpu.hpp"
#include "ilegacysim/userland_hle.hpp"
#include "ilegacysim/wifi_state.hpp"

namespace ilegacysim {
namespace {

constexpr std::string_view core_telephony_image{
    "/System/Library/Frameworks/CoreTelephony.framework/CoreTelephony"};
constexpr std::string_view core_foundation_image{
    "/System/Library/Frameworks/CoreFoundation.framework/CoreFoundation"};
constexpr std::string_view springboard_image{
    "/System/Library/CoreServices/SpringBoard.app/SpringBoard"};
constexpr std::string_view application_directory{"Applications/"};
constexpr std::uint32_t springboard_telephony_checked_in_method{0x0002a9f4U};
constexpr std::string_view offline_sim_status_export{
    "_kCTSIMSupportSIMStatusNotInserted"};
constexpr std::string_view create_call_from_info{
    "__CTCallCreateFromCallInfo"};
constexpr std::string_view copy_cf_string{"_CFStringGetCString"};
constexpr std::string_view default_telephony_center{
    "_CTTelephonyCenterGetDefault"};
constexpr std::uint32_t cf_string_encoding_utf8{0x08000100U};
constexpr std::uint32_t call_argument_words{8U};
constexpr std::uint32_t call_argument_bytes{
    call_argument_words * sizeof(std::uint32_t)};
constexpr std::size_t maximum_dialed_number_bytes{256U};

// These APIs have scalar return values in iPhoneOS 1.0. An offline handset
// has no data attachment, active data context, signal, airplane mode, or calls.
constexpr std::array<std::string_view, 4> offline_scalar_queries{
    "_CTRegistrationGetDataAttached",
    "_CTRegistrationGetDataContextActive",
    "_CTGetSignalStrength",
    "_CTGetCurrentCallCount",
};

constexpr std::array<std::string_view, 3> observer_operations{
    "_CTTelephonyCenterAddObserver",
    "_CTTelephonyCenterRemoveObserver",
    "_CTTelephonyCenterRemoveEveryObserver",
};

// These server queries normally write a retained CFString through argument 2.
// The offline profile has a genuine connection object but no CommCenter-backed
// values, so terminate the queries at this HLE boundary.
constexpr std::array<std::string_view, 6> offline_server_string_queries{
    "__CTServerConnectionCopyFirmwareVersion",
    "__CTServerConnectionCopySIMIdentity",
    "__CTServerConnectionCopyMobileIdentity",
    "__CTServerConnectionGetMobileSubscriberCountryCode",
    "__CTServerConnectionCopyOperatorName",
    "__CTServerConnectionCopyProviderName",
};

bool is_offline_ui_client(const UserlandHleCall& call) {
    return call.image_loaded(springboard_image) ||
           call.image_loaded_beneath(application_directory);
}

std::uint32_t exported_object(UserlandHleCall& call,
                              std::string_view variable) {
    const auto address = call.symbol_address(variable);
    return address ? call.memory().read32(*address).value_or(0) : 0;
}

void return_firmware_object(UserlandHleCall& call,
                            std::string_view variable) {
    if (!is_offline_ui_client(call)) {
        call.resume_original();
        return;
    }
    call.set_return(exported_object(call, variable));
}

void return_empty_server_string(UserlandHleCall& call) {
    if (!is_offline_ui_client(call)) {
        call.resume_original();
        return;
    }
    const auto result = call.argument(0);
    const auto value_output = call.argument(2);
    if (result == 0 || value_output == 0 ||
        !call.write32(result, 0) || !call.write32(result + 4U, 0) ||
        !call.write32(value_output, 0)) {
        call.set_return(0);
        return;
    }
    call.set_return(result);
}

void return_server_value(UserlandHleCall& call, std::uint32_t value) {
    if (!is_offline_ui_client(call)) {
        call.resume_original();
        return;
    }
    const auto result = call.argument(0);
    const auto value_output = call.argument(2);
    if (result == 0 || value_output == 0 ||
        !call.write32(result, 0) || !call.write32(result + 4U, 0) ||
        !call.write32(value_output, value)) {
        call.set_return(0);
        return;
    }
    call.set_return(result);
}

void return_server_success(UserlandHleCall& call) {
    if (!is_offline_ui_client(call)) {
        call.resume_original();
        return;
    }
    const auto result = call.argument(0);
    if (result == 0 || !call.write32(result, 0) ||
        !call.write32(result + 4U, 0)) {
        call.set_return(0);
        return;
    }
    call.set_return(result);
}

void return_server_failure(UserlandHleCall& call, std::uint32_t result,
                           std::uint32_t call_output,
                           std::uint32_t error) {
    if (call_output != 0) {
        static_cast<void>(call.write32(call_output, 0));
    }
    if (result != 0) {
        static_cast<void>(call.write32(result, 1));
        static_cast<void>(call.write32(result + 4U, error));
    }
    call.set_return(result);
}

struct OfflineCallRequest {
    std::uint32_t result{};
    std::uint32_t call_output{};
    std::uint32_t number{};
    std::uint32_t dial_identifier{};
    std::uint32_t caller_stack{};
    std::uint32_t sequence{};
    std::uint32_t address{};
};

bool create_firmware_call(UserlandHleCall& call,
                          const OfflineCallRequest& request,
                          std::uint32_t allocator_source) {
    if (allocator_source == 0 ||
        request.caller_stack < call_argument_bytes) {
        return false;
    }
    const auto empty = call.intern_string("");
    if (empty == 0) return false;

    const auto call_stack =
        request.caller_stack - call_argument_bytes;
    const std::array<std::uint32_t, call_argument_words> arguments{
        0x694c5349U,             // Fourth CFUUID word.
        request.address,        // Dialed address.
        empty,                  // No network-provided display name.
        0U,                     // Start time is unknown while dialing.
        0U,                     // Connected duration.
        request.sequence,       // Stable call identifier.
        2U,                     // Outgoing, dialing call state.
        request.dial_identifier,
    };
    for (std::size_t index = 0; index < arguments.size(); ++index) {
        if (!call.write32(
                call_stack +
                    static_cast<std::uint32_t>(index) *
                        static_cast<std::uint32_t>(
                            sizeof(std::uint32_t)),
                arguments[index])) {
            return false;
        }
    }

    auto& registers = call.cpu().registers();
    registers[0] = allocator_source;
    registers[1] = 0x694c6567U;
    registers[2] = call.process_id();
    registers[3] = request.sequence;
    registers[13] = call_stack;
    if (call.call_guest_function(
            create_call_from_info,
            [request](UserlandHleCall& completed) {
                completed.cpu().registers()[13] =
                    request.caller_stack;
                const auto created =
                    completed.cpu().registers()[0];
                if (created != 0 &&
                    completed.write32(
                        request.call_output, created) &&
                    completed.write32(request.result, 0) &&
                    completed.write32(
                        request.result + 4U, 0)) {
                    completed.set_return(request.result);
                    return;
                }
                return_server_failure(
                    completed, request.result,
                    request.call_output, 12U);
            })) {
        return true;
    }
    registers[13] = request.caller_stack;
    return false;
}

}  // namespace

void register_core_telephony_hle(UserlandHleRegistry& registry) {
    const auto wifi_state = std::make_shared<WifiState>();
    register_core_telephony_hle(
        registry, [wifi_state] { return wifi_state; });
}

void register_core_telephony_hle(
    UserlandHleRegistry& registry, WifiStateProvider wifi_state,
    std::function<void(const WifiSnapshot&, const WifiSnapshot&)>
        wifi_state_changed) {
    registry.register_guest_function(
        std::string{core_foundation_image},
        std::string{copy_cf_string});
    // Every stock UI process shares one offline compatibility backend. Match
    // the application directory instead of maintaining a bundle whitelist,
    // while allowing CommCenter and other system daemons to keep their native
    // internal object lifecycles.
    // Without CommCenter, the stock method would keep the status controller in
    // its pre-check-in state and pass nil into SBStatusBarNoServiceView. Report
    // the already-implemented offline CoreTelephony boundary as checked in so
    // SpringBoard chooses and localizes its own NO_SERVICE/SEARCHING string.
    registry.register_address(
        std::string{springboard_image},
        springboard_telephony_checked_in_method,
        "-[SBStatusBarController telephonyControllerCheckedIn]",
        [](UserlandHleCall& call) { call.set_return(1); });

    // Preserve the firmware's CTCall CFRuntime object and MobilePhone flow.
    // Only adapt the missing baseband service reply into the same call-info
    // record that a real CommCenter response would have produced.
    const auto next_call_identifier =
        std::make_shared<std::atomic<std::uint32_t>>(1U);
    registry.register_function(
        std::string{core_telephony_image},
        "__CTServerConnectionCreateCall",
        [next_call_identifier](UserlandHleCall& call) {
            if (!is_offline_ui_client(call)) {
                call.resume_original();
                return;
            }

            OfflineCallRequest request{
                call.argument(0),
                call.argument(4),
                call.argument(2),
                call.argument(3),
                call.cpu().registers()[13],
                next_call_identifier->fetch_add(
                    1U, std::memory_order_relaxed),
                call.allocate_data(maximum_dialed_number_bytes, 1U),
            };
            if (request.result == 0 || request.call_output == 0 ||
                request.number == 0 || request.address == 0) {
                return_server_failure(
                    call, request.result, request.call_output, 22U);
                return;
            }

            auto& registers = call.cpu().registers();
            registers[0] = request.number;
            registers[1] = request.address;
            registers[2] =
                static_cast<std::uint32_t>(
                    maximum_dialed_number_bytes);
            registers[3] = cf_string_encoding_utf8;
            if (!call.call_guest_function(
                    copy_cf_string,
                    [request](UserlandHleCall& copied) {
                        if (copied.cpu().registers()[0] == 0) {
                            return_server_failure(
                                copied, request.result,
                                request.call_output, 22U);
                            return;
                        }

                        if (!copied.call_guest_function(
                                default_telephony_center,
                                [request](UserlandHleCall& centered) {
                                    const auto center =
                                        centered.cpu().registers()[0];
                                    if (!create_firmware_call(
                                            centered, request, center)) {
                                        return_server_failure(
                                            centered, request.result,
                                            request.call_output, 38U);
                                    }
                                })) {
                            return_server_failure(
                                copied, request.result,
                            request.call_output, 38U);
                        }
                    })) {
                return_server_failure(
                    call, request.result, request.call_output, 38U);
            }
        });

    for (const auto symbol : offline_scalar_queries) {
        registry.register_function(
            std::string{core_telephony_image}, std::string{symbol},
            [](UserlandHleCall& call) {
                if (is_offline_ui_client(call)) {
                    call.set_return(0);
                } else {
                    call.resume_original();
                }
            });
    }
    registry.register_function(
        std::string{core_telephony_image}, "_CTPowerGetAirplaneMode",
        [wifi_state](UserlandHleCall& call) {
            if (!is_offline_ui_client(call)) {
                call.resume_original();
                return;
            }
            const auto state = wifi_state ? wifi_state() : nullptr;
            call.set_return(
                state && state->snapshot().airplane_mode ? 1U : 0U);
        });
    registry.register_function(
        std::string{core_telephony_image}, "_CTPowerSetAirplaneMode",
        [wifi_state, wifi_state_changed](UserlandHleCall& call) {
            if (!is_offline_ui_client(call)) {
                call.resume_original();
                return;
            }
            const auto state = wifi_state ? wifi_state() : nullptr;
            if (!state) {
                call.set_return(0);
                return;
            }
            const auto before = state->snapshot();
            static_cast<void>(
                state->set_airplane_mode(call.argument(0) != 0));
            const auto after = state->snapshot();
            if (wifi_state_changed) wifi_state_changed(before, after);
            call.set_return(0);
        });

    registry.register_function(
        std::string{core_telephony_image}, "_CTRegistrationGetStatus",
        [](UserlandHleCall& call) {
            return_firmware_object(
                call, "_kCTRegistrationStatusNotRegistered");
        });
    registry.register_function(
        std::string{core_telephony_image}, "_CTSIMSupportGetSIMStatus",
        [](UserlandHleCall& call) {
            return_firmware_object(call, offline_sim_status_export);
        });
    for (const auto symbol : offline_server_string_queries) {
        registry.register_function(
            std::string{core_telephony_image}, std::string{symbol},
            [](UserlandHleCall& call) { return_empty_server_string(call); });
    }
    registry.register_function(
        std::string{core_telephony_image},
        "__CTServerConnectionGetSIMStatus",
        [](UserlandHleCall& call) {
            return_server_value(
                call, exported_object(call, offline_sim_status_export));
        });

    // Let the firmware construct its genuine CFRuntime server connection,
    // including the dedicated receive right exposed by GetPort. The launchd
    // Mach service exists even when the baseband transport is parked. Offline
    // clients may subscribe locally, but registration cannot produce remote
    // CommCenter updates and is therefore a successful no-op.
    for (const auto symbol : {
             "__CTServerConnectionRegisterForNotification",
             "__CTServerConnectionUnregisterForNotification",
         }) {
        registry.register_function(
            std::string{core_telephony_image}, symbol,
            [](UserlandHleCall& call) { return_server_success(call); });
    }
    registry.register_function(
        std::string{core_telephony_image},
        "__CTServerConnectionSetCTMMode",
        [](UserlandHleCall& call) { return_server_success(call); });
    registry.register_function(
        std::string{core_telephony_image},
        "__CTServerConnectionNetworkTimeUpdatesAllowed",
        [](UserlandHleCall& call) { return_server_value(call, 0); });
    // Automatic carrier selection eventually dereferences the connection's
    // CommCenter CFMachPort. Baseband transport is intentionally parked, so
    // terminate the request at the user-space CoreTelephony boundary.
    registry.register_function(
        std::string{core_telephony_image},
        "__CTServerConnectionSelectNetwork",
        [](UserlandHleCall& call) { return_server_success(call); });

    // Let the firmware construct a genuine CFRuntime telephony-center object,
    // while suppressing only its attempt to establish the unavailable
    // CommCenter/baseband channel. Every client can retain/release it normally.
    registry.register_function(
        std::string{core_telephony_image}, "__EstablishServerConnection",
        [](UserlandHleCall& call) {
            if (is_offline_ui_client(call)) {
                call.set_return(0);
            } else {
                call.resume_original();
            }
        });
    registry.register_function(
        std::string{core_telephony_image}, "__KillServerConnection",
        [](UserlandHleCall& call) {
            if (is_offline_ui_client(call)) {
                call.set_return(0);
            } else {
                call.resume_original();
            }
        });
    for (const auto symbol : observer_operations) {
        registry.register_function(
            std::string{core_telephony_image}, std::string{symbol},
            [](UserlandHleCall& call) {
                if (is_offline_ui_client(call)) {
                    call.set_return(0);
                } else {
                    call.resume_original();
                }
            });
    }
}

}  // namespace ilegacysim
