cmake_minimum_required(VERSION 3.24)

project(iLEmu VERSION 0.1.0 LANGUAGES C CXX)

option(ILEMU_ENABLE_IPO
    "Optimize across translation units in Release and RelWithDebInfo builds" ON)
if(ILEMU_ENABLE_IPO)
    include(CheckIPOSupported)
    check_ipo_supported(RESULT ILEMU_IPO_SUPPORTED
        OUTPUT ILEMU_IPO_ERROR LANGUAGES C CXX)
    if(ILEMU_IPO_SUPPORTED)
        # Startup spends much of its time in the JIT's small IR and emission
        # helpers. Let the compiler inline these across library boundaries.
        set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASE ON)
        set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELWITHDEBINFO ON)
    else()
        message(STATUS "IPO unavailable; using standard optimization: ${ILEMU_IPO_ERROR}")
    endif()
endif()

include(CTest)
find_package(Threads REQUIRED)
option(ILEMU_ENABLE_SDL2 "Build the optional SDL2 display backend" ON)
option(ILEMU_ENABLE_VULKAN
    "Build the optional Vulkan GLES renderer" ON)
if(ILEMU_ENABLE_VULKAN)
    find_package(Vulkan QUIET)
endif()
option(ILEMU_ENABLE_DYNARMIC_TEST_EMIT_FAILURE
    "Compile Dynarmic portable-emit fault injection for tests" OFF)
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
set(ILEMU_TESTS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/tests" CACHE PATH
    "Compatibility test source tree; may live in the surrounding workspace")

set(ILEMU_DYNARMIC_DIR "${CMAKE_CURRENT_SOURCE_DIR}/external/dynarmic" CACHE PATH
    "Path to the patched MC-XiaoXiao Dynarmic checkout")
set(ILEMU_BOOST_DIR "${CMAKE_CURRENT_SOURCE_DIR}/external/ext-boost" CACHE PATH
    "Path to the matching ext-boost checkout")

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
    OUTPUT_VARIABLE ILEMU_DYNARMIC_CHECKED_OUT_REVISION
    ERROR_QUIET
    OUTPUT_STRIP_TRAILING_WHITESPACE)
if(ILEMU_DYNARMIC_REVISION_RESULT EQUAL 0 AND
   NOT ILEMU_DYNARMIC_CHECKED_OUT_REVISION STREQUAL "")
    set(ILEMU_DYNARMIC_SELECTED_REVISION
        "${ILEMU_DYNARMIC_CHECKED_OUT_REVISION}" CACHE INTERNAL
        "Dynarmic revision observed during configure" FORCE)
    message(STATUS
        "Using Dynarmic ${ILEMU_DYNARMIC_CHECKED_OUT_REVISION} "
        "with the parent-repository iLEmu test patch")
else()
    set(ILEMU_DYNARMIC_SELECTED_REVISION "unknown" CACHE INTERNAL
        "Dynarmic revision observed during configure" FORCE)
endif()

# The iLEmu test hook is carried as a reproducible parent-repository patch.
# Applying it is idempotent for reused build directories.
set(_ilemu_dynarmic_test_patch
    "${CMAKE_CURRENT_SOURCE_DIR}/tools/dynarmic_ilemu_test_emit_failure.patch")
if(NOT EXISTS "${_ilemu_dynarmic_test_patch}")
    message(FATAL_ERROR
        "Missing Dynarmic test patch: ${_ilemu_dynarmic_test_patch}")
endif()
execute_process(
    COMMAND git apply --check --unidiff-zero "${_ilemu_dynarmic_test_patch}"
    WORKING_DIRECTORY "${ILEMU_DYNARMIC_DIR}"
    RESULT_VARIABLE _ilemu_dynarmic_test_patch_check
    OUTPUT_QUIET ERROR_VARIABLE _ilemu_dynarmic_test_patch_error)
if(_ilemu_dynarmic_test_patch_check EQUAL 0)
    execute_process(
        COMMAND git apply --unidiff-zero "${_ilemu_dynarmic_test_patch}"
        WORKING_DIRECTORY "${ILEMU_DYNARMIC_DIR}"
        RESULT_VARIABLE _ilemu_dynarmic_test_patch_apply
        OUTPUT_QUIET ERROR_VARIABLE _ilemu_dynarmic_test_patch_error)
    if(NOT _ilemu_dynarmic_test_patch_apply EQUAL 0)
        message(FATAL_ERROR
            "Failed to apply the Dynarmic test patch: "
            "${_ilemu_dynarmic_test_patch_error}")
    endif()
else()
    execute_process(
        COMMAND git apply --check --unidiff-zero --reverse
                "${_ilemu_dynarmic_test_patch}"
        WORKING_DIRECTORY "${ILEMU_DYNARMIC_DIR}"
        RESULT_VARIABLE _ilemu_dynarmic_test_patch_reverse_check
        OUTPUT_QUIET ERROR_VARIABLE _ilemu_dynarmic_test_patch_reverse_error)
    if(NOT _ilemu_dynarmic_test_patch_reverse_check EQUAL 0)
        message(FATAL_ERROR
            "Dynarmic source is incompatible with the iLEmu test patch: "
            "${_ilemu_dynarmic_test_patch_error}; "
            "${_ilemu_dynarmic_test_patch_reverse_error}")
    endif()
endif()

# Force Dynarmic's FindBoost invocation to use the matching header-only tree.
set(Boost_NO_SYSTEM_PATHS ON CACHE BOOL "" FORCE)
set(BOOST_ROOT "${ILEMU_BOOST_DIR}" CACHE PATH "" FORCE)
set(Boost_INCLUDE_DIR "${ILEMU_BOOST_DIR}" CACHE PATH "" FORCE)
set(Boost_INCLUDE_DIRS "${ILEMU_BOOST_DIR}" CACHE PATH "" FORCE)

set(DYNARMIC_FRONTENDS A32 CACHE STRING "" FORCE)
set(DYNARMIC_TESTS OFF CACHE BOOL "" FORCE)
set(DYNARMIC_ENABLE_ILEMU_TEST_EMIT_FAILURE
    "${ILEMU_ENABLE_DYNARMIC_TEST_EMIT_FAILURE}" CACHE BOOL "" FORCE)
set(DYNARMIC_USE_BUNDLED_EXTERNALS ON CACHE BOOL "" FORCE)
set(DYNARMIC_USE_LLVM OFF CACHE BOOL "" FORCE)
set(DYNARMIC_WARNINGS_AS_ERRORS OFF CACHE BOOL "" FORCE)
set(DYNARMIC_USE_PRECOMPILED_HEADERS OFF CACHE BOOL "" FORCE)

# Public headers are kept with their owning module. The list is intentionally
# explicit so every target, including the out-of-tree workspace tests, sees
# the same <module>/<name>.hpp include spelling. A major module owns one
# include root; implementation subdirectories do not add public roots.
# Persistent portable-IR artifacts must not cross a different Dynarmic build.
# Include the actual revision and any deliberate local checkout changes in the
# artifact key so a custom development checkout cannot reuse stale artifacts.
set(ILEMU_DYNARMIC_BUILD_FINGERPRINT 0x0ULL)
if(ILEMU_DYNARMIC_REVISION_RESULT EQUAL 0 AND
   NOT ILEMU_DYNARMIC_CHECKED_OUT_REVISION STREQUAL "")
    execute_process(
        COMMAND git diff --no-ext-diff --binary
        WORKING_DIRECTORY "${ILEMU_DYNARMIC_DIR}"
        OUTPUT_VARIABLE ILEMU_DYNARMIC_WORKTREE_DIFF
        ERROR_QUIET)
    string(SHA256 ILEMU_DYNARMIC_BUILD_HASH
           "${ILEMU_DYNARMIC_CHECKED_OUT_REVISION}\n${ILEMU_DYNARMIC_WORKTREE_DIFF}")
    string(SUBSTRING "${ILEMU_DYNARMIC_BUILD_HASH}" 0 16
           ILEMU_DYNARMIC_BUILD_HASH_PREFIX)
    set(ILEMU_DYNARMIC_BUILD_FINGERPRINT
        "0x${ILEMU_DYNARMIC_BUILD_HASH_PREFIX}ULL")
endif()

add_subdirectory("${ILEMU_DYNARMIC_DIR}" external/dynarmic EXCLUDE_FROM_ALL)

function(ilemu_configure_target target)
    target_compile_features(${target} PUBLIC cxx_std_20)
    target_compile_options(${target} PRIVATE
        $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall;-Wextra;-Wpedantic;-Wconversion;-Wshadow>
    )
endfunction()

function(ilemu_configure_module target)
    file(RELATIVE_PATH _ilemu_module_path "${PROJECT_SOURCE_DIR}"
        "${CMAKE_CURRENT_SOURCE_DIR}")
    if(_ilemu_module_path MATCHES "^src/([^/]+)")
        set(_ilemu_module "${CMAKE_MATCH_1}")
        set(_ilemu_include_root
            "${PROJECT_SOURCE_DIR}/src/${_ilemu_module}/include")
    elseif(_ilemu_module_path MATCHES "^app($|/)")
        set(_ilemu_module app)
        set(_ilemu_include_root "${PROJECT_SOURCE_DIR}/app/include")
    else()
        message(FATAL_ERROR "No owning module for target ${target}")
    endif()
    target_include_directories(${target} PUBLIC
        "$<BUILD_INTERFACE:${_ilemu_include_root}>")
    # Kernel implementation subtargets share the kernel's declared header
    # contract, without linking back to the parent implementation library.
    if(_ilemu_module STREQUAL "kernel")
        target_link_libraries(${target} PUBLIC iLEmu::kernel_api)
    endif()
    ilemu_configure_target(${target})
endfunction()

add_subdirectory(src/mach)
add_subdirectory(src/foundation)
add_subdirectory(src/device_state)
add_subdirectory(src/filesystem)
add_subdirectory(src/network)
add_subdirectory(src/storage)
add_subdirectory(src/telephony)
add_subdirectory(src/media)
add_subdirectory(src/bluetooth)
add_subdirectory(src/crypto)
add_subdirectory(src/graphics)
add_subdirectory(src/kernel)
add_subdirectory(src/debug)
add_subdirectory(src/host)

add_library(ilemu_core INTERFACE)
add_library(iLEmu::core ALIAS ilemu_core)
target_link_libraries(ilemu_core INTERFACE
    iLEmu::kernel
    iLEmu::debug)

add_subdirectory(src/runtime)
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
