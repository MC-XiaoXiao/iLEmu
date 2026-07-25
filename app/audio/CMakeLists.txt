add_library(ilemu_app_audio STATIC
    ffmpeg_audio_decoder.cpp
    sdl_audio_sink.cpp)
add_library(iLEmu::app_audio ALIAS ilemu_app_audio)
ilemu_configure_module(ilemu_app_audio)
target_include_directories(ilemu_app_audio PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>)
target_link_libraries(ilemu_app_audio PUBLIC iLEmu::media)
if(TARGET PkgConfig::SDL2)
    target_link_libraries(ilemu_app_audio PUBLIC PkgConfig::SDL2)
    target_compile_definitions(ilemu_app_audio PUBLIC
        ILEMU_HAS_SDL2=1)
endif()
if(TARGET PkgConfig::FFMPEG)
    target_link_libraries(ilemu_app_audio PUBLIC PkgConfig::FFMPEG)
    target_compile_definitions(ilemu_app_audio PUBLIC
        ILEMU_HAS_FFMPEG=1)
endif()
