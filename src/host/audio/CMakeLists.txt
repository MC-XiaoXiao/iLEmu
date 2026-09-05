add_library(ilemu_host_audio STATIC
    ffmpeg_audio_decoder.cpp
    sdl_audio_sink.cpp)
add_library(iLEmu::host_audio ALIAS ilemu_host_audio)
ilemu_configure_module(ilemu_host_audio)
target_link_libraries(ilemu_host_audio PUBLIC iLEmu::media)
if(TARGET PkgConfig::SDL2)
    target_link_libraries(ilemu_host_audio PRIVATE PkgConfig::SDL2)
    target_compile_definitions(ilemu_host_audio PRIVATE
        ILEMU_HAS_SDL2=1)
endif()
if(TARGET PkgConfig::FFMPEG)
    target_link_libraries(ilemu_host_audio PRIVATE PkgConfig::FFMPEG)
    target_compile_definitions(ilemu_host_audio PRIVATE
        ILEMU_HAS_FFMPEG=1)
endif()
