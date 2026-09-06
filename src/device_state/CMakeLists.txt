# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.

add_library(ilemu_device_state STATIC
    darwin_firmware_identity.cpp
    darwin_kernel_configuration.cpp
    darwin_kernel_identity.cpp
    graphics_services_capabilities.cpp
    launchd_job_catalog.cpp
    lockdown_state.cpp
    network_preferences.cpp)
add_library(iLEmu::device_state ALIAS ilemu_device_state)
ilemu_configure_module(ilemu_device_state)
target_link_libraries(ilemu_device_state PUBLIC
    iLEmu::foundation iLEmu::network)
if(TARGET PkgConfig::PLIST)
    target_link_libraries(ilemu_device_state PRIVATE PkgConfig::PLIST)
    target_compile_definitions(ilemu_device_state PRIVATE
        ILEMU_HAS_LIBPLIST=1)
endif()
