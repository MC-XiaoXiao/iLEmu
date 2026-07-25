add_library(ilemu_media STATIC
    audio.cpp
    audio_toolbox_hle.cpp
    celestial_volume_protocol.cpp
    core_audio_hle.cpp
    media_library_service.cpp)
add_library(iLEmu::media ALIAS ilemu_media)
ilemu_configure_module(ilemu_media)
target_link_libraries(ilemu_media PUBLIC iLEmu::foundation)
if(TARGET PkgConfig::PLIST)
    target_link_libraries(ilemu_media PRIVATE PkgConfig::PLIST)
    target_compile_definitions(ilemu_media PRIVATE
        ILEMU_HAS_LIBPLIST=1)
endif()
