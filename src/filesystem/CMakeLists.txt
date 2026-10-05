# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.

if(NOT TARGET OpenSSL::Crypto)
    find_package(OpenSSL REQUIRED COMPONENTS Crypto)
endif()

add_library(ilemu_filesystem STATIC host_file_rename.cpp hfs_metadata.cpp hfs_bulk_attributes.cpp hfs_volume_layout.cpp hfs_catalog_lookup.cpp)
add_library(iLEmu::filesystem ALIAS ilemu_filesystem)
ilemu_configure_module(ilemu_filesystem)
target_link_libraries(ilemu_filesystem PUBLIC iLEmu::foundation PRIVATE OpenSSL::Crypto)
