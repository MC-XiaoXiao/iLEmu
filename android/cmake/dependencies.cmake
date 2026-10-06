# Android dependencies are built for the NDK target, never discovered through
# the build machine's pkg-config database. Keep archives pinned and verified.
include(FetchContent)
include(ExternalProject)

set(ILEMU_ANDROID_DOWNLOAD_DIR "${CMAKE_BINARY_DIR}/downloads" CACHE PATH
    "Reusable cache for verified Android dependency archives")
set(ILEMU_DEPENDENCY_JOBS 4 CACHE STRING "Parallel jobs for make dependencies")
find_program(ILEMU_MAKE make REQUIRED)
find_package(Perl REQUIRED)
get_filename_component(_android_tools "${CMAKE_C_COMPILER}" DIRECTORY)
set(_android_prefix "${CMAKE_BINARY_DIR}/dependencies/install")
file(MAKE_DIRECTORY "${_android_prefix}/include")
include("${CMAKE_CURRENT_LIST_DIR}/ffmpeg.cmake")

ExternalProject_Add(android_openssl
    URL https://github.com/openssl/openssl/releases/download/openssl-3.5.7/openssl-3.5.7.tar.gz
    URL_HASH SHA256=a8c0d28a529ca480f9f36cf5792e2cd21984552a3c8e4aa11a24aa31aeac98e8
    DOWNLOAD_DIR "${ILEMU_ANDROID_DOWNLOAD_DIR}"
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    CONFIGURE_COMMAND "${CMAKE_COMMAND}" -E env
        "ANDROID_NDK_ROOT=${CMAKE_ANDROID_NDK}"
        "PATH=${_android_tools}:$ENV{PATH}"
        "${PERL_EXECUTABLE}" <SOURCE_DIR>/Configure android-arm64
        "--prefix=${_android_prefix}" --libdir=lib
        "-D__ANDROID_API__=${ANDROID_PLATFORM_LEVEL}" -fPIC
        no-shared no-tests no-module
    BUILD_COMMAND "${CMAKE_COMMAND}" -E env
        "PATH=${_android_tools}:$ENV{PATH}"
        "${ILEMU_MAKE}" "-j${ILEMU_DEPENDENCY_JOBS}" build_libs
    INSTALL_COMMAND "${ILEMU_MAKE}" install_dev
    BUILD_BYPRODUCTS "${_android_prefix}/lib/libcrypto.a"
    LOG_CONFIGURE TRUE LOG_BUILD TRUE LOG_INSTALL TRUE)
add_library(OpenSSL::Crypto STATIC IMPORTED GLOBAL)
set_target_properties(OpenSSL::Crypto PROPERTIES
    IMPORTED_LOCATION "${_android_prefix}/lib/libcrypto.a"
    INTERFACE_INCLUDE_DIRECTORIES "${_android_prefix}/include"
    INTERFACE_LINK_LIBRARIES "${CMAKE_DL_LIBS}")
add_dependencies(OpenSSL::Crypto android_openssl)

ExternalProject_Add(android_plist
    URL https://github.com/libimobiledevice/libplist/releases/download/2.8.0/libplist-2.8.0.tar.bz2
    URL_HASH SHA256=b1f59f7634c58b2481325a23ff4e3bf51574a42d868cbe466d2b39b04550752a
    DOWNLOAD_DIR "${ILEMU_ANDROID_DOWNLOAD_DIR}"
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    CONFIGURE_COMMAND "${CMAKE_COMMAND}" -E env
        "CC=${_android_tools}/aarch64-linux-android${ANDROID_PLATFORM_LEVEL}-clang"
        "CXX=${_android_tools}/aarch64-linux-android${ANDROID_PLATFORM_LEVEL}-clang++"
        "AR=${CMAKE_AR}" "RANLIB=${CMAKE_RANLIB}"
        "CFLAGS=-O2 -fPIC" "CXXFLAGS=-O2 -fPIC" "PKG_CONFIG=false"
        <SOURCE_DIR>/configure --host=aarch64-linux-android
        "--prefix=${_android_prefix}" --disable-shared --enable-static
        --without-cython --without-tools --without-tests
    BUILD_COMMAND "${ILEMU_MAKE}" "-j${ILEMU_DEPENDENCY_JOBS}"
    INSTALL_COMMAND "${ILEMU_MAKE}" install
    BUILD_BYPRODUCTS "${_android_prefix}/lib/libplist-2.0.a"
    LOG_CONFIGURE TRUE LOG_BUILD TRUE LOG_INSTALL TRUE)
add_library(iLEmu::plist STATIC IMPORTED GLOBAL)
set_target_properties(iLEmu::plist PROPERTIES
    IMPORTED_LOCATION "${_android_prefix}/lib/libplist-2.0.a"
    INTERFACE_INCLUDE_DIRECTORIES "${_android_prefix}/include")
add_dependencies(iLEmu::plist android_plist)

# zlib is part of the public NDK API. libpng and libjpeg remain private static
# dependencies of the emulator library.
set(PNG_SHARED OFF CACHE BOOL "" FORCE)
set(PNG_STATIC ON CACHE BOOL "" FORCE)
set(PNG_TESTS OFF CACHE BOOL "" FORCE)
set(PNG_TOOLS OFF CACHE BOOL "" FORCE)
FetchContent_Declare(android_png
    URL https://github.com/pnggroup/libpng/archive/refs/tags/v1.6.59.tar.gz
    URL_HASH SHA256=2540302a1844ad2b2b501977abecfa850f265f97b78f065a712ab4074a89f5b5
    DOWNLOAD_NAME libpng-1.6.59.tar.gz
    DOWNLOAD_DIR "${ILEMU_ANDROID_DOWNLOAD_DIR}"
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    EXCLUDE_FROM_ALL)
FetchContent_MakeAvailable(android_png)
add_library(PNG::PNG ALIAS png_static)

ExternalProject_Add(android_jpeg
    URL https://github.com/libjpeg-turbo/libjpeg-turbo/releases/download/3.2.0/libjpeg-turbo-3.2.0.tar.gz
    URL_HASH SHA256=6f30092cef9fb839779646608f4ee14ae3cbac989c47fa05e841b0841f09878e
    DOWNLOAD_DIR "${ILEMU_ANDROID_DOWNLOAD_DIR}"
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    CMAKE_ARGS
        "-DCMAKE_TOOLCHAIN_FILE=${CMAKE_TOOLCHAIN_FILE}"
        "-DANDROID_ABI=${ANDROID_ABI}"
        "-DANDROID_PLATFORM=${ANDROID_PLATFORM}"
        "-DCMAKE_INSTALL_PREFIX=${_android_prefix}"
        -DCMAKE_INSTALL_LIBDIR=lib -DCMAKE_BUILD_TYPE=Release
        -DCMAKE_POSITION_INDEPENDENT_CODE=ON
        -DENABLE_SHARED=OFF -DENABLE_STATIC=ON
        -DWITH_TURBOJPEG=OFF -DWITH_TOOLS=OFF -DWITH_TESTS=OFF
    BUILD_COMMAND "${CMAKE_COMMAND}" --build <BINARY_DIR>
        --parallel "${ILEMU_DEPENDENCY_JOBS}"
    BUILD_BYPRODUCTS "${_android_prefix}/lib/libjpeg.a"
    LOG_CONFIGURE TRUE LOG_BUILD TRUE LOG_INSTALL TRUE)
add_library(JPEG::JPEG STATIC IMPORTED GLOBAL)
set_target_properties(JPEG::JPEG PROPERTIES
    IMPORTED_LOCATION "${_android_prefix}/lib/libjpeg.a"
    INTERFACE_INCLUDE_DIRECTORIES "${_android_prefix}/include")
add_dependencies(JPEG::JPEG android_jpeg)

set(SDL_SHARED ON CACHE BOOL "" FORCE)
set(SDL_STATIC OFF CACHE BOOL "" FORCE)
set(SDL_TEST OFF CACHE BOOL "" FORCE)
set(SDL2_DISABLE_INSTALL ON CACHE BOOL "" FORCE)
set(SDL2_DISABLE_SDL2MAIN ON CACHE BOOL "" FORCE)
add_subdirectory("${ILEMU_REPOSITORY_ROOT}/external/sdl"
    "${CMAKE_BINARY_DIR}/dependencies/sdl" EXCLUDE_FROM_ALL)
