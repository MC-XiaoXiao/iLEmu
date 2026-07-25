add_library(ilemu_device_state STATIC
    lockdown_profile.cpp
    network_preferences.cpp)
add_library(iLEmu::device_state ALIAS ilemu_device_state)
ilemu_configure_module(ilemu_device_state)
if(TARGET PkgConfig::PLIST)
    target_link_libraries(ilemu_device_state PRIVATE PkgConfig::PLIST)
    target_compile_definitions(ilemu_device_state PRIVATE
        ILEMU_HAS_LIBPLIST=1)
endif()
