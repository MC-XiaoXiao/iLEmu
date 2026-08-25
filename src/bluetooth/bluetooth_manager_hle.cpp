#include "ilemu/bluetooth_manager_hle.hpp"

#include <string>
#include <string_view>

#include "ilemu/userland_hle.hpp"

namespace ilemu {
namespace {

    constexpr std::string_view mobile_bluetooth_image {
        "/MobileBluetooth.framework/MobileBluetooth"
    };

} // namespace

void register_bluetooth_manager_hle(UserlandHleRegistry& registry)
{
    registry.register_function(std::string { mobile_bluetooth_image },
        "_BTSessionAttachWithRunLoop", [](UserlandHleCall& call) {
            // There is no emulated Bluetooth controller. Report attachment
            // failure at the public MobileBluetooth backend boundary so every
            // daemon and framework takes its native no-Bluetooth fallback.
            call.set_return(1);
        });
}

} // namespace ilemu
