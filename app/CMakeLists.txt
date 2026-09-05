add_subdirectory(control)
add_subdirectory(sdl)

add_executable(ilemu main.cpp desktop_host.cpp)
target_link_libraries(ilemu PRIVATE
    iLEmu::host
    iLEmu::app_control
    iLEmu::app_sdl
    iLEmu::runtime
    iLEmu::core
    iLEmu::device_state
    iLEmu::kernel_bsd_baseband_device)
set_target_properties(ilemu PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}")
