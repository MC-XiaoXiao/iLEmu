# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.

add_library(ilemu_storage STATIC
    app_support_hle.cpp
    lockdown_hle.cpp)
add_library(iLEmu::storage ALIAS ilemu_storage)
ilemu_configure_module(ilemu_storage)
target_link_libraries(ilemu_storage PUBLIC
    iLEmu::foundation iLEmu::device_state)
