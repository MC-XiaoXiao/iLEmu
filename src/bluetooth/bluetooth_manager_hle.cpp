#include "ilemu/bluetooth_manager_hle.hpp"

#include <string>
#include <string_view>

#include "ilemu/userland_hle.hpp"

namespace ilemu {
namespace {

constexpr std::string_view bluetooth_manager_image{
    "/System/Library/Frameworks/BluetoothManager.framework/BluetoothManager"};
constexpr std::string_view mobile_bluetooth_image{
    "/System/Library/Frameworks/MobileBluetooth.framework/MobileBluetooth"};
constexpr std::string_view springboard_image{
    "/System/Library/CoreServices/SpringBoard.app/SpringBoard"};
constexpr std::string_view application_directory{"Applications/"};

}  // namespace

void register_bluetooth_manager_hle(UserlandHleRegistry& registry) {
    registry.register_function(
        std::string{mobile_bluetooth_image}, "_BTSessionAttachWithRunLoop",
        [](UserlandHleCall& call) {
            // There is no emulated Bluetooth controller. Report attachment
            // failure at the public MobileBluetooth backend boundary so every
            // daemon and framework takes its native no-Bluetooth fallback.
            call.set_return(1);
        });
    registry.register_function(
        std::string{bluetooth_manager_image},
        "-[BluetoothManager _setupSession]",
        [](UserlandHleCall& call) {
            if (!call.image_loaded(springboard_image) &&
                !call.image_loaded_beneath(application_directory)) {
                call.resume_original();
                return;
            }
            // All UI clients share the same offline Bluetooth boundary; the
            // daemon itself retains its native setup path.
            call.set_return(0);
        });
}

}  // namespace ilemu
