cmake_minimum_required(VERSION 3.24)
project(iLEmuAndroid LANGUAGES C CXX)

if(NOT ANDROID OR NOT ANDROID_ABI STREQUAL "arm64-v8a")
    message(FATAL_ERROR "The Android frontend currently supports arm64-v8a only")
endif()
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(ILEMU_BUILD_JOBS 8 CACHE STRING "Maximum concurrent native compilations")
set_property(GLOBAL PROPERTY JOB_POOLS "native_compile=${ILEMU_BUILD_JOBS};native_link=1")
set(CMAKE_JOB_POOL_COMPILE native_compile)
set(CMAKE_JOB_POOL_LINK native_link)
set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(ILEMU_ENABLE_IPO OFF CACHE BOOL "" FORCE)
set(ILEMU_ENABLE_SDL2 ON CACHE BOOL "" FORCE)
set(ILEMU_ENABLE_VULKAN ON CACHE BOOL "" FORCE)
get_filename_component(ILEMU_REPOSITORY_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../../../.." ABSOLUTE)
find_program(Vulkan_GLSLC_EXECUTABLE glslc
    HINTS "${CMAKE_ANDROID_NDK}/shader-tools/${ANDROID_HOST_TAG}"
    REQUIRED NO_CMAKE_FIND_ROOT_PATH)
include("${ILEMU_REPOSITORY_ROOT}/android/cmake/dependencies.cmake")
add_subdirectory("${ILEMU_REPOSITORY_ROOT}" "${CMAKE_BINARY_DIR}/core" EXCLUDE_FROM_ALL)
# The pinned Boost headers still inherit std::unary_function. libc++ provides
# this compatibility switch for C++17-and-later consumers of those headers.
target_compile_definitions(dynarmic PUBLIC
    _LIBCPP_ENABLE_CXX17_REMOVED_UNARY_BINARY_FUNCTION)

add_library(ilemu SHARED android_main.cpp android_host.cpp)
target_link_libraries(ilemu PRIVATE
    iLEmu::app_commands iLEmu::app_control iLEmu::app_sdl
    SDL2::SDL2 android log)
target_compile_options(ilemu PRIVATE -Wall -Wextra -Wpedantic)
