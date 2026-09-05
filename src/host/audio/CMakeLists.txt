add_library(ilemu_host_audio STATIC
    ffmpeg_audio_decoder.cpp)
add_library(iLEmu::host_audio ALIAS ilemu_host_audio)
ilemu_configure_module(ilemu_host_audio)
target_link_libraries(ilemu_host_audio PUBLIC iLEmu::media)
if(TARGET PkgConfig::FFMPEG)
    target_link_libraries(ilemu_host_audio PRIVATE PkgConfig::FFMPEG)
    target_compile_definitions(ilemu_host_audio PRIVATE
        ILEMU_HAS_FFMPEG=1)
endif()
