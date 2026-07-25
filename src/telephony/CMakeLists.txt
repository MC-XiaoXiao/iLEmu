add_library(ilemu_telephony STATIC
    core_telephony_hle.cpp)
add_library(iLEmu::telephony ALIAS ilemu_telephony)
ilemu_configure_module(ilemu_telephony)
target_link_libraries(ilemu_telephony PUBLIC
    iLEmu::foundation
    iLEmu::network)
