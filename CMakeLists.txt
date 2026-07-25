cmake_minimum_required(VERSION 3.24)

project(iLEmu VERSION 0.1.0 LANGUAGES C CXX)

include(CTest)
find_package(Threads REQUIRED)
option(ILEMU_ENABLE_SDL2 "Build the optional SDL2 display backend" ON)
option(ILEMU_ENABLE_VULKAN
    "Build the optional Vulkan GLES renderer" ON)
find_package(PkgConfig QUIET)
if(ILEMU_ENABLE_SDL2)
    if(PkgConfig_FOUND)
        pkg_check_modules(SDL2 QUIET IMPORTED_TARGET sdl2)
    endif()
endif()
if(PkgConfig_FOUND)
    pkg_check_modules(FFMPEG QUIET IMPORTED_TARGET
        libavformat libavcodec libavutil libswresample)
    pkg_check_modules(PLIST QUIET IMPORTED_TARGET libplist-2.0)
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(ILEMU_DYNARMIC_DIR "${CMAKE_CURRENT_SOURCE_DIR}/external/dynarmic" CACHE PATH
    "Path to the MC-XiaoXiao Dynarmic checkout")
set(ILEMU_BOOST_DIR "${CMAKE_CURRENT_SOURCE_DIR}/external/ext-boost" CACHE PATH
    "Path to the matching ext-boost checkout")

if(NOT EXISTS "${ILEMU_DYNARMIC_DIR}/CMakeLists.txt")
    message(FATAL_ERROR
        "Dynarmic was not found at ${ILEMU_DYNARMIC_DIR}. "
        "Clone https://github.com/MC-XiaoXiao/dynarmic.git there or set ILEMU_DYNARMIC_DIR.")
endif()
if(NOT EXISTS "${ILEMU_BOOST_DIR}/boost/version.hpp")
    message(FATAL_ERROR
        "ext-boost was not found at ${ILEMU_BOOST_DIR}. "
        "Clone https://github.com/MC-XiaoXiao/ext-boost.git there or set ILEMU_BOOST_DIR.")
endif()

# Force Dynarmic's FindBoost invocation to use the matching header-only tree.
set(Boost_NO_SYSTEM_PATHS ON CACHE BOOL "" FORCE)
set(BOOST_ROOT "${ILEMU_BOOST_DIR}" CACHE PATH "" FORCE)
set(Boost_INCLUDE_DIR "${ILEMU_BOOST_DIR}" CACHE PATH "" FORCE)
set(Boost_INCLUDE_DIRS "${ILEMU_BOOST_DIR}" CACHE PATH "" FORCE)

set(DYNARMIC_FRONTENDS A32 CACHE STRING "" FORCE)
set(DYNARMIC_TESTS OFF CACHE BOOL "" FORCE)
set(DYNARMIC_USE_BUNDLED_EXTERNALS ON CACHE BOOL "" FORCE)
set(DYNARMIC_USE_LLVM OFF CACHE BOOL "" FORCE)
set(DYNARMIC_WARNINGS_AS_ERRORS OFF CACHE BOOL "" FORCE)
set(DYNARMIC_USE_PRECOMPILED_HEADERS OFF CACHE BOOL "" FORCE)
add_subdirectory("${ILEMU_DYNARMIC_DIR}" external/dynarmic EXCLUDE_FROM_ALL)

function(ilemu_configure_module target)
    target_include_directories(${target} PUBLIC
        $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
    )
    target_compile_features(${target} PUBLIC cxx_std_20)
    target_compile_options(${target} PRIVATE
        $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall;-Wextra;-Wpedantic;-Wconversion;-Wshadow>
    )
endfunction()

add_subdirectory(tools/mig)
add_subdirectory(src)
add_subdirectory(app)

add_subdirectory(tools/guest)

if(BUILD_TESTING)
    add_subdirectory(tests)
endif()
