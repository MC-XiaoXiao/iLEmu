# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.

add_library(ilemu_filesystem STATIC hfs_metadata.cpp hfs_volume_layout.cpp)
add_library(iLEmu::filesystem ALIAS ilemu_filesystem)
ilemu_configure_module(ilemu_filesystem)
target_link_libraries(ilemu_filesystem PUBLIC iLEmu::foundation)
