# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.

add_library(ilemu_network STATIC
    apple80211_event_stream.cpp
    apple80211_hle.cpp
    darwin_network_abi.cpp
    darwin_route_socket.cpp
    dns_configuration_hle.cpp
    host_network.cpp
    system_configuration_hle.cpp
    wifi_state.cpp)
add_library(iLEmu::network ALIAS ilemu_network)
ilemu_configure_module(ilemu_network)
target_link_libraries(ilemu_network PUBLIC iLEmu::foundation)
