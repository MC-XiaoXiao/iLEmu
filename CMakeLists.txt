cmake_minimum_required(VERSION 3.24)

project(iLEmu VERSION 0.1.0 LANGUAGES C CXX)

include(CTest)
find_package(Threads REQUIRED)
option(ILEMU_ENABLE_SDL2 "Build the optional SDL2 display backend" ON)
option(ILEMU_ENABLE_VULKAN
    "Build the optional Vulkan GLES renderer" ON)
find_package(PkgConfig QUIET)
if(ILEMU_ENABLE_SDL2 AND PkgConfig_FOUND)
    pkg_check_modules(SDL2 QUIET IMPORTED_TARGET sdl2)
endif()
if(PkgConfig_FOUND)
    pkg_check_modules(FFMPEG QUIET IMPORTED_TARGET
        libavformat libavcodec libavutil libswresample)
    pkg_check_modules(PLIST QUIET IMPORTED_TARGET libplist-2.0)
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# The source repository is usable on its own. A workspace checkout can pass
# these paths from its top-level CMake project without changing this entry
# point.
set(ILEMU_WORKSPACE_DIR "${CMAKE_CURRENT_SOURCE_DIR}" CACHE PATH
    "Workspace containing optional tools, tests, docs, and firmware data")
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../sources")
    set(_ilemu_default_sources_dir "${CMAKE_CURRENT_SOURCE_DIR}/../sources")
else()
    set(_ilemu_default_sources_dir "${CMAKE_CURRENT_SOURCE_DIR}/sources")
endif()
set(ILEMU_SOURCES_DIR "${_ilemu_default_sources_dir}" CACHE PATH
    "XNU/configd/launchd sources used by the MIG metadata generator")
set(ILEMU_TESTS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/tests" CACHE PATH
    "Compatibility test source tree; may live in the surrounding workspace")

set(ILEMU_DYNARMIC_DIR "${CMAKE_CURRENT_SOURCE_DIR}/external/dynarmic" CACHE PATH
    "Path to the patched MC-XiaoXiao Dynarmic checkout")
set(ILEMU_BOOST_DIR "${CMAKE_CURRENT_SOURCE_DIR}/external/ext-boost" CACHE PATH
    "Path to the matching ext-boost checkout")
set(ILEMU_DYNARMIC_PATCHED_REVISION
    "3e3f37dcc1601f01809f2efe3320580baa8c1855" CACHE STRING
    "Expected Dynarmic revision for the x86_64/A32 iLEmu backend")

if(NOT EXISTS "${ILEMU_DYNARMIC_DIR}/CMakeLists.txt")
    message(FATAL_ERROR
        "Dynarmic was not found at ${ILEMU_DYNARMIC_DIR}. "
        "Initialize the submodule or set ILEMU_DYNARMIC_DIR.")
endif()
if(NOT EXISTS "${ILEMU_BOOST_DIR}/boost/version.hpp")
    message(FATAL_ERROR
        "ext-boost was not found at ${ILEMU_BOOST_DIR}. "
        "Initialize the submodule or set ILEMU_BOOST_DIR.")
endif()

execute_process(
    COMMAND git rev-parse HEAD
    WORKING_DIRECTORY "${ILEMU_DYNARMIC_DIR}"
    RESULT_VARIABLE ILEMU_DYNARMIC_REVISION_RESULT
    OUTPUT_VARIABLE ILEMU_DYNARMIC_REVISION
    ERROR_QUIET
    OUTPUT_STRIP_TRAILING_WHITESPACE)
if(ILEMU_DYNARMIC_REVISION_RESULT EQUAL 0 AND
   NOT ILEMU_DYNARMIC_REVISION STREQUAL "")
    if(NOT ILEMU_DYNARMIC_REVISION STREQUAL
       "${ILEMU_DYNARMIC_PATCHED_REVISION}")
        message(WARNING
            "Dynarmic revision ${ILEMU_DYNARMIC_REVISION} is not the "
            "repository's patched revision "
            "${ILEMU_DYNARMIC_PATCHED_REVISION}. Continuing with the "
            "explicitly selected checkout.")
    else()
        message(STATUS
            "Using Dynarmic ${ILEMU_DYNARMIC_REVISION} with the iLEmu "
            "patch stack committed in the dependency repository")
    endif()
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

# Public headers are kept with their owning module. The list is intentionally
# explicit so every target, including the out-of-tree workspace tests, sees
# the same stable ilemu/<name>.hpp include spelling.
set(ILEMU_PROJECT_INCLUDE_DIRS
    "${CMAKE_CURRENT_SOURCE_DIR}/src/foundation/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/device_state/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/mach/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/filesystem/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/network/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/storage/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/telephony/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/media/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/bluetooth/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/graphics/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/kernel/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/debug/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/app/audio/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/app/control/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/app/runtime/include")

# Persistent portable-IR artifacts must not cross a different Dynarmic build.
# Include the pinned revision and any deliberate local checkout changes in the
# artifact key so a custom development checkout cannot reuse stale artifacts.
set(ILEMU_DYNARMIC_BUILD_FINGERPRINT 0x0ULL)
if(ILEMU_DYNARMIC_REVISION_RESULT EQUAL 0 AND
   NOT ILEMU_DYNARMIC_REVISION STREQUAL "")
    execute_process(
        COMMAND git diff --no-ext-diff --binary
        WORKING_DIRECTORY "${ILEMU_DYNARMIC_DIR}"
        OUTPUT_VARIABLE ILEMU_DYNARMIC_WORKTREE_DIFF
        ERROR_QUIET)
    string(SHA256 ILEMU_DYNARMIC_BUILD_HASH
           "${ILEMU_DYNARMIC_REVISION}\n${ILEMU_DYNARMIC_WORKTREE_DIFF}")
    string(SUBSTRING "${ILEMU_DYNARMIC_BUILD_HASH}" 0 16
           ILEMU_DYNARMIC_BUILD_HASH_PREFIX)
    set(ILEMU_DYNARMIC_BUILD_FINGERPRINT
        "0x${ILEMU_DYNARMIC_BUILD_HASH_PREFIX}ULL")
endif()

add_subdirectory("${ILEMU_DYNARMIC_DIR}" external/dynarmic EXCLUDE_FROM_ALL)

function(ilemu_configure_module target)
    target_include_directories(${target} PUBLIC
        ${ILEMU_PROJECT_INCLUDE_DIRS})
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
    if(EXISTS "${ILEMU_TESTS_DIR}/CMakeLists.txt")
        add_subdirectory("${ILEMU_TESTS_DIR}" tests)
    else()
        message(STATUS
            "Compatibility tests are not present at ${ILEMU_TESTS_DIR}; "
            "set ILEMU_TESTS_DIR to enable them.")
    endif()
endif()
