# Build the existing host decoder against target libraries, independently of
# the desktop pkg-config database. Shared libraries keep FFmpeg replaceable.
set(_ffmpeg_libraries avformat avcodec swresample avutil)
set(_ffmpeg_byproducts)
foreach(_library IN LISTS _ffmpeg_libraries)
    list(APPEND _ffmpeg_byproducts "${_android_prefix}/lib/lib${_library}.so")
endforeach()

ExternalProject_Add(android_ffmpeg
    URL https://ffmpeg.org/releases/ffmpeg-8.0.3.tar.xz
    URL_HASH SHA256=6136812ea6d4e68bdba27e33c2a94382711cdf4f8602ffef056ff792bd6f9818
    DOWNLOAD_DIR "${ILEMU_ANDROID_DOWNLOAD_DIR}"
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    CONFIGURE_COMMAND <SOURCE_DIR>/configure
        "--prefix=${_android_prefix}"
        --target-os=android --arch=aarch64 --enable-cross-compile
        "--cc=${_android_tools}/aarch64-linux-android${ANDROID_PLATFORM_LEVEL}-clang"
        "--cxx=${_android_tools}/aarch64-linux-android${ANDROID_PLATFORM_LEVEL}-clang++"
        "--ar=${CMAKE_AR}" "--ranlib=${CMAKE_RANLIB}"
        "--nm=${_android_tools}/llvm-nm" "--strip=${_android_tools}/llvm-strip"
        --enable-shared --disable-static --enable-pic
        --extra-ldflags=-Wl,-z,max-page-size=16384
        --disable-autodetect --disable-programs --disable-doc --disable-debug
        --disable-avdevice --disable-avfilter --disable-swscale
        --disable-network --disable-encoders --disable-muxers --disable-hwaccels
    BUILD_COMMAND "${ILEMU_MAKE}" "-j${ILEMU_DEPENDENCY_JOBS}"
    INSTALL_COMMAND "${ILEMU_MAKE}" install
    BUILD_BYPRODUCTS ${_ffmpeg_byproducts}
    LOG_CONFIGURE TRUE LOG_BUILD TRUE LOG_INSTALL TRUE)

add_library(ilemu_android_ffmpeg INTERFACE)
add_library(iLEmu::ffmpeg ALIAS ilemu_android_ffmpeg)
foreach(_library IN LISTS _ffmpeg_libraries)
    add_library(ilemu_ffmpeg_${_library} SHARED IMPORTED GLOBAL)
    set_target_properties(ilemu_ffmpeg_${_library} PROPERTIES
        IMPORTED_LOCATION "${_android_prefix}/lib/lib${_library}.so"
        INTERFACE_INCLUDE_DIRECTORIES "${_android_prefix}/include")
    add_dependencies(ilemu_ffmpeg_${_library} android_ffmpeg)
    target_link_libraries(ilemu_android_ffmpeg INTERFACE ilemu_ffmpeg_${_library})
endforeach()
