add_subdirectory(audio)
add_subdirectory(graphics)

add_library(ilemu_host INTERFACE)
add_library(iLEmu::host ALIAS ilemu_host)
target_link_libraries(ilemu_host INTERFACE
    iLEmu::host_audio
    iLEmu::host_graphics)
