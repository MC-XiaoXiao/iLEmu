# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.

add_library(ilemu_runtime STATIC
    boot_options.cpp
    emulator_session.cpp
    realtime_pacer.cpp
    resource_policy.cpp
    session_catalog.cpp
    session_diagnostics.cpp
    input/live_button_scheduler.cpp
    input/live_touch_scheduler.cpp)
add_library(iLEmu::runtime ALIAS ilemu_runtime)
ilemu_configure_module(ilemu_runtime)
target_link_libraries(ilemu_runtime
    PUBLIC iLEmu::foundation iLEmu::device_state iLEmu::graphics iLEmu::network
    PRIVATE iLEmu::kernel iLEmu::debug iLEmu::crypto)
