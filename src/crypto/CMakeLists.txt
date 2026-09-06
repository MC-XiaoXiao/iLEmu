# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.

find_package(OpenSSL REQUIRED COMPONENTS Crypto)

add_library(ilemu_crypto STATIC
    core_crypto_hle.cpp
    prime_field_layout.cpp
    host/key_store.cpp
    host/big_number.cpp)
add_library(iLEmu::crypto ALIAS ilemu_crypto)
ilemu_configure_module(ilemu_crypto)
target_include_directories(ilemu_crypto PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(ilemu_crypto PUBLIC iLEmu::foundation
    PRIVATE OpenSSL::Crypto)
