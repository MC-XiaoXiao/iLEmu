add_subdirectory(audio)
add_subdirectory(graphics)

add_library(ilemu_host_system STATIC system/resource_usage.cpp)
add_library(iLEmu::host_system ALIAS ilemu_host_system)
ilemu_configure_module(ilemu_host_system)
target_link_libraries(ilemu_host_system PUBLIC iLEmu::foundation)

add_library(ilemu_host INTERFACE)
add_library(iLEmu::host ALIAS ilemu_host)
target_link_libraries(ilemu_host INTERFACE
    iLEmu::host_audio
    iLEmu::host_graphics
    iLEmu::host_system)
