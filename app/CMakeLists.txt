add_subdirectory(audio)
add_subdirectory(control)
add_subdirectory(runtime)

add_executable(ilemu main.cpp)
target_link_libraries(ilemu PRIVATE
    iLEmu::app_audio
    iLEmu::app_control
    iLEmu::app_runtime
    iLEmu::core
    iLEmu::device_state
    iLEmu::kernel_bsd_baseband_device)
set_target_properties(ilemu PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}")
