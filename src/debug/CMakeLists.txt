# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.

add_library(ilemu_debug STATIC gdb_rsp.cpp)
add_library(iLEmu::debug ALIAS ilemu_debug)
ilemu_configure_module(ilemu_debug)
target_link_libraries(ilemu_debug PUBLIC iLEmu::foundation)
