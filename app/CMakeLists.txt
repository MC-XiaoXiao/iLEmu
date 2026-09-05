add_library(ilemu_app_control STATIC control/live_control.cpp)
add_library(iLEmu::app_control ALIAS ilemu_app_control)
ilemu_configure_module(ilemu_app_control)
target_link_libraries(ilemu_app_control PUBLIC
    iLEmu::foundation iLEmu::debug)

add_library(ilemu_app_sdl STATIC
    sdl/sdl_display.cpp
    sdl/sdl_input.cpp
    sdl/sdl_audio_sink.cpp)
add_library(iLEmu::app_sdl ALIAS ilemu_app_sdl)
ilemu_configure_module(ilemu_app_sdl)
target_link_libraries(ilemu_app_sdl PUBLIC
    iLEmu::graphics
    iLEmu::media)
if(TARGET PkgConfig::SDL2)
    target_link_libraries(ilemu_app_sdl PRIVATE PkgConfig::SDL2)
    target_compile_definitions(ilemu_app_sdl PRIVATE ILEMU_HAS_SDL2=1)
    if(ILEMU_ENABLE_VULKAN AND Vulkan_FOUND AND Vulkan_GLSLC_EXECUTABLE)
        target_link_libraries(ilemu_app_sdl PRIVATE Vulkan::Headers)
        target_compile_definitions(ilemu_app_sdl PRIVATE ILEMU_HAS_VULKAN=1)
    endif()
endif()

add_executable(ilemu main.cpp desktop_host.cpp abi_command.cpp)
target_link_libraries(ilemu PRIVATE
    iLEmu::host
    iLEmu::app_control
    iLEmu::app_sdl
    iLEmu::runtime)
set_target_properties(ilemu PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}")
