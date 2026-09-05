add_library(ilemu_app_sdl STATIC
    sdl_display.cpp
    sdl_input.cpp
    sdl_audio_sink.cpp)
add_library(iLEmu::app_sdl ALIAS ilemu_app_sdl)
ilemu_configure_module(ilemu_app_sdl)
target_link_libraries(ilemu_app_sdl PUBLIC
    iLEmu::graphics
    iLEmu::media)
if(TARGET PkgConfig::SDL2)
    target_link_libraries(ilemu_app_sdl PRIVATE PkgConfig::SDL2)
    target_compile_definitions(ilemu_app_sdl PRIVATE ILEMU_HAS_SDL2=1)
endif()
