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

# The shared-monitor reservation resolver is a small, reproducible extension
# of the pinned Dynarmic checkout. Keep the dependency checkout itself
# ignored, but apply the tracked patch at configure time so a clean checkout
# cannot silently fall back to raw Guest-VA reservations. The reverse check
# makes repeated CMake reconfiguration idempotent.
set(ILEMU_DYNARMIC_RESOLVER_PATCH
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/patches/dynarmic-exclusive-resolver.patch")
set(ILEMU_DYNARMIC_PORTABLE_IR_PATCH
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/patches/dynarmic-portable-ir.patch")
set(ILEMU_DYNARMIC_A32_STABLE_LINK_PATCH
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/patches/dynarmic-a32-stable-links.patch")
set(ILEMU_DYNARMIC_NORMAL_STABLE_LINK_PATCH
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/patches/dynarmic-normal-stable-links.patch")
set(ILEMU_DYNARMIC_SHARED_RSB_PATCH
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/patches/dynarmic-shared-rsb.patch")
execute_process(
    COMMAND git apply --check "${ILEMU_DYNARMIC_PORTABLE_IR_PATCH}"
    WORKING_DIRECTORY "${ILEMU_DYNARMIC_DIR}"
    RESULT_VARIABLE ILEMU_DYNARMIC_COMPLETE_PATCH_CHECK
    OUTPUT_QUIET
    ERROR_QUIET)
if(ILEMU_DYNARMIC_COMPLETE_PATCH_CHECK EQUAL 0)
    # The complete portable-IR patch contains the resolver changes. Leave a
    # pristine checkout untouched here so the complete patch can be applied
    # atomically below instead of making both patches partially applicable.
    message(STATUS
        "Deferring the Dynarmic resolver to the complete portable-IR patch")
else()
    execute_process(
        COMMAND git apply --check "${ILEMU_DYNARMIC_RESOLVER_PATCH}"
        WORKING_DIRECTORY "${ILEMU_DYNARMIC_DIR}"
        RESULT_VARIABLE ILEMU_DYNARMIC_PATCH_CHECK
        OUTPUT_VARIABLE ILEMU_DYNARMIC_PATCH_OUTPUT
        ERROR_VARIABLE ILEMU_DYNARMIC_PATCH_ERROR)
    if(ILEMU_DYNARMIC_PATCH_CHECK EQUAL 0)
    execute_process(
        COMMAND git apply "${ILEMU_DYNARMIC_RESOLVER_PATCH}"
        WORKING_DIRECTORY "${ILEMU_DYNARMIC_DIR}"
        RESULT_VARIABLE ILEMU_DYNARMIC_PATCH_APPLY
        OUTPUT_VARIABLE ILEMU_DYNARMIC_PATCH_APPLY_OUTPUT
        ERROR_VARIABLE ILEMU_DYNARMIC_PATCH_APPLY_ERROR)
    if(NOT ILEMU_DYNARMIC_PATCH_APPLY EQUAL 0)
        message(FATAL_ERROR
            "Failed to apply the Dynarmic reservation resolver patch: "
            "${ILEMU_DYNARMIC_PATCH_APPLY_ERROR}")
    endif()
    else()
    execute_process(
        COMMAND git apply --reverse --check "${ILEMU_DYNARMIC_RESOLVER_PATCH}"
        WORKING_DIRECTORY "${ILEMU_DYNARMIC_DIR}"
        RESULT_VARIABLE ILEMU_DYNARMIC_PATCH_REVERSE_CHECK
        OUTPUT_VARIABLE ILEMU_DYNARMIC_PATCH_REVERSE_OUTPUT
        ERROR_VARIABLE ILEMU_DYNARMIC_PATCH_REVERSE_ERROR)
    execute_process(
        COMMAND git grep -q "HasAddressResolver"
            -- src/dynarmic/interface/exclusive_monitor.h
        WORKING_DIRECTORY "${ILEMU_DYNARMIC_DIR}"
        RESULT_VARIABLE ILEMU_DYNARMIC_RESOLVER_API_CHECK)
    execute_process(
        COMMAND git grep -q "ResolveAddress"
            -- src/dynarmic/backend/x64/exclusive_monitor.cpp
        WORKING_DIRECTORY "${ILEMU_DYNARMIC_DIR}"
        RESULT_VARIABLE ILEMU_DYNARMIC_RESOLVER_BACKEND_CHECK)
    if(NOT ILEMU_DYNARMIC_PATCH_REVERSE_CHECK EQUAL 0 AND
       NOT (ILEMU_DYNARMIC_RESOLVER_API_CHECK EQUAL 0 AND
            ILEMU_DYNARMIC_RESOLVER_BACKEND_CHECK EQUAL 0))
        message(FATAL_ERROR
            "Dynarmic reservation resolver patch is neither applicable nor "
            "already applied. Apply it to the checkout or use a compatible "
            "Dynarmic revision. git apply reported: "
            "${ILEMU_DYNARMIC_PATCH_ERROR}")
    elseif(NOT ILEMU_DYNARMIC_PATCH_REVERSE_CHECK EQUAL 0)
        message(STATUS
            "Dynarmic evolved checkout already provides the reservation "
            "resolver API; skipping the older tracked patch")
    endif()
    endif()
endif()

# Expose only the narrow Dynarmic boundary needed by the project-owned
# portable-IR serializer. The dependency checkout remains ignored; this
# tracked patch keeps clean configurations reproducible and idempotent.
execute_process(
    COMMAND git apply --check "${ILEMU_DYNARMIC_PORTABLE_IR_PATCH}"
    WORKING_DIRECTORY "${ILEMU_DYNARMIC_DIR}"
    RESULT_VARIABLE ILEMU_DYNARMIC_PORTABLE_IR_CHECK
    OUTPUT_VARIABLE ILEMU_DYNARMIC_PORTABLE_IR_OUTPUT
    ERROR_VARIABLE ILEMU_DYNARMIC_PORTABLE_IR_ERROR)
if(ILEMU_DYNARMIC_PORTABLE_IR_CHECK EQUAL 0)
    execute_process(
        COMMAND git apply "${ILEMU_DYNARMIC_PORTABLE_IR_PATCH}"
        WORKING_DIRECTORY "${ILEMU_DYNARMIC_DIR}"
        RESULT_VARIABLE ILEMU_DYNARMIC_PORTABLE_IR_APPLY
        OUTPUT_VARIABLE ILEMU_DYNARMIC_PORTABLE_IR_APPLY_OUTPUT
        ERROR_VARIABLE ILEMU_DYNARMIC_PORTABLE_IR_APPLY_ERROR)
    if(NOT ILEMU_DYNARMIC_PORTABLE_IR_APPLY EQUAL 0)
        message(FATAL_ERROR
            "Failed to apply the Dynarmic portable-IR patch: "
            "${ILEMU_DYNARMIC_PORTABLE_IR_APPLY_ERROR}")
    endif()
else()
    execute_process(
        COMMAND git apply --reverse --check "${ILEMU_DYNARMIC_PORTABLE_IR_PATCH}"
        WORKING_DIRECTORY "${ILEMU_DYNARMIC_DIR}"
        RESULT_VARIABLE ILEMU_DYNARMIC_PORTABLE_IR_REVERSE_CHECK
        OUTPUT_VARIABLE ILEMU_DYNARMIC_PORTABLE_IR_REVERSE_OUTPUT
        ERROR_VARIABLE ILEMU_DYNARMIC_PORTABLE_IR_REVERSE_ERROR)
    execute_process(
        COMMAND git grep -q "GeneratePortableIR"
            -- src/dynarmic/interface/A32/a32.h
        WORKING_DIRECTORY "${ILEMU_DYNARMIC_DIR}"
        RESULT_VARIABLE ILEMU_DYNARMIC_PORTABLE_IR_API_CHECK)
    execute_process(
        COMMAND git grep -q "PortableIRGenerated"
            -- src/dynarmic/interface/A32/config.h
        WORKING_DIRECTORY "${ILEMU_DYNARMIC_DIR}"
        RESULT_VARIABLE ILEMU_DYNARMIC_PORTABLE_IR_CALLBACK_CHECK)
    execute_process(
        COMMAND git grep -q "bool Precompile"
            -- src/dynarmic/interface/A32/a32.h
        WORKING_DIRECTORY "${ILEMU_DYNARMIC_DIR}"
        RESULT_VARIABLE ILEMU_DYNARMIC_PRECOMPILE_CHECK)
    if(NOT ILEMU_DYNARMIC_PORTABLE_IR_REVERSE_CHECK EQUAL 0 AND
       NOT (ILEMU_DYNARMIC_PORTABLE_IR_API_CHECK EQUAL 0 AND
            ILEMU_DYNARMIC_PORTABLE_IR_CALLBACK_CHECK EQUAL 0 AND
            ILEMU_DYNARMIC_PRECOMPILE_CHECK EQUAL 0))
        message(FATAL_ERROR
            "Dynarmic portable-IR patch is neither applicable nor already "
            "applied. git apply reported: "
            "${ILEMU_DYNARMIC_PORTABLE_IR_ERROR}")
    elseif(NOT ILEMU_DYNARMIC_PORTABLE_IR_REVERSE_CHECK EQUAL 0)
        message(STATUS
            "Dynarmic evolved checkout already provides the portable-IR "
            "API; skipping the older tracked patch")
    endif()
endif()

# Keep shared A32 generated blocks immutable while allowing a generation-local
# executor slot to resolve stable links directly. The patch is incremental to
# the portable-IR patch above and is deliberately strict on clean checkouts.
execute_process(
    COMMAND git apply --check "${ILEMU_DYNARMIC_A32_STABLE_LINK_PATCH}"
    WORKING_DIRECTORY "${ILEMU_DYNARMIC_DIR}"
    RESULT_VARIABLE ILEMU_DYNARMIC_A32_STABLE_LINK_CHECK
    OUTPUT_VARIABLE ILEMU_DYNARMIC_A32_STABLE_LINK_OUTPUT
    ERROR_VARIABLE ILEMU_DYNARMIC_A32_STABLE_LINK_ERROR)
if(ILEMU_DYNARMIC_A32_STABLE_LINK_CHECK EQUAL 0)
    execute_process(
        COMMAND git apply "${ILEMU_DYNARMIC_A32_STABLE_LINK_PATCH}"
        WORKING_DIRECTORY "${ILEMU_DYNARMIC_DIR}"
        RESULT_VARIABLE ILEMU_DYNARMIC_A32_STABLE_LINK_APPLY
        OUTPUT_VARIABLE ILEMU_DYNARMIC_A32_STABLE_LINK_APPLY_OUTPUT
        ERROR_VARIABLE ILEMU_DYNARMIC_A32_STABLE_LINK_APPLY_ERROR)
    if(NOT ILEMU_DYNARMIC_A32_STABLE_LINK_APPLY EQUAL 0)
        message(FATAL_ERROR
            "Failed to apply the Dynarmic A32 stable-link patch: "
            "${ILEMU_DYNARMIC_A32_STABLE_LINK_APPLY_ERROR}")
    endif()
else()
    execute_process(
        COMMAND git apply --reverse --check
            "${ILEMU_DYNARMIC_A32_STABLE_LINK_PATCH}"
        WORKING_DIRECTORY "${ILEMU_DYNARMIC_DIR}"
        RESULT_VARIABLE ILEMU_DYNARMIC_A32_STABLE_LINK_REVERSE_CHECK
        OUTPUT_VARIABLE ILEMU_DYNARMIC_A32_STABLE_LINK_REVERSE_OUTPUT
        ERROR_VARIABLE ILEMU_DYNARMIC_A32_STABLE_LINK_REVERSE_ERROR)
    if(NOT ILEMU_DYNARMIC_A32_STABLE_LINK_REVERSE_CHECK EQUAL 0)
        # The shared-RSB patch intentionally replaces the old stable-link
        # body, so the older patch's reverse hunk is no longer exact after
        # the complete tracked stack has been applied.
        execute_process(
            COMMAND git apply --reverse --check
                "${ILEMU_DYNARMIC_SHARED_RSB_PATCH}"
            WORKING_DIRECTORY "${ILEMU_DYNARMIC_DIR}"
            RESULT_VARIABLE ILEMU_DYNARMIC_SHARED_RSB_ALREADY_APPLIED
            OUTPUT_VARIABLE ILEMU_DYNARMIC_SHARED_RSB_ALREADY_OUTPUT
            ERROR_VARIABLE ILEMU_DYNARMIC_SHARED_RSB_ALREADY_ERROR)
        if(NOT ILEMU_DYNARMIC_SHARED_RSB_ALREADY_APPLIED EQUAL 0)
            message(FATAL_ERROR
                "Dynarmic A32 stable-link patch is neither applicable nor "
                "already applied. git apply reported: "
                "${ILEMU_DYNARMIC_A32_STABLE_LINK_ERROR}")
        endif()
    endif()
endif()

# Extend the same executor-local stable-slot probe to ordinary LinkBlock
# success branches while preserving their cycle/halt guards and patch sites.
execute_process(
    COMMAND git apply --unidiff-zero --check "${ILEMU_DYNARMIC_NORMAL_STABLE_LINK_PATCH}"
    WORKING_DIRECTORY "${ILEMU_DYNARMIC_DIR}"
    RESULT_VARIABLE ILEMU_DYNARMIC_NORMAL_STABLE_LINK_CHECK
    OUTPUT_VARIABLE ILEMU_DYNARMIC_NORMAL_STABLE_LINK_OUTPUT
    ERROR_VARIABLE ILEMU_DYNARMIC_NORMAL_STABLE_LINK_ERROR)
if(ILEMU_DYNARMIC_NORMAL_STABLE_LINK_CHECK EQUAL 0)
    execute_process(
        COMMAND git apply --unidiff-zero "${ILEMU_DYNARMIC_NORMAL_STABLE_LINK_PATCH}"
        WORKING_DIRECTORY "${ILEMU_DYNARMIC_DIR}"
        RESULT_VARIABLE ILEMU_DYNARMIC_NORMAL_STABLE_LINK_APPLY
        OUTPUT_VARIABLE ILEMU_DYNARMIC_NORMAL_STABLE_LINK_APPLY_OUTPUT
        ERROR_VARIABLE ILEMU_DYNARMIC_NORMAL_STABLE_LINK_APPLY_ERROR)
    if(NOT ILEMU_DYNARMIC_NORMAL_STABLE_LINK_APPLY EQUAL 0)
        message(FATAL_ERROR
            "Failed to apply the Dynarmic ordinary stable-link patch: "
            "${ILEMU_DYNARMIC_NORMAL_STABLE_LINK_APPLY_ERROR}")
    endif()
else()
    execute_process(
        COMMAND git apply --unidiff-zero --reverse --check
            "${ILEMU_DYNARMIC_NORMAL_STABLE_LINK_PATCH}"
        WORKING_DIRECTORY "${ILEMU_DYNARMIC_DIR}"
        RESULT_VARIABLE ILEMU_DYNARMIC_NORMAL_STABLE_LINK_REVERSE_CHECK
        OUTPUT_VARIABLE ILEMU_DYNARMIC_NORMAL_STABLE_LINK_REVERSE_OUTPUT
        ERROR_VARIABLE ILEMU_DYNARMIC_NORMAL_STABLE_LINK_REVERSE_ERROR)
    if(NOT ILEMU_DYNARMIC_NORMAL_STABLE_LINK_REVERSE_CHECK EQUAL 0)
        message(FATAL_ERROR
            "Dynarmic ordinary stable-link patch is neither applicable nor "
            "already applied. git apply reported: "
            "${ILEMU_DYNARMIC_NORMAL_STABLE_LINK_ERROR}")
    endif()
endif()

# Keep RSB entries generation-local in the shared A32/x64 backend. The
# executor-owned stable table is the only source of native return pointers.
execute_process(
    COMMAND git apply --check "${ILEMU_DYNARMIC_SHARED_RSB_PATCH}"
    WORKING_DIRECTORY "${ILEMU_DYNARMIC_DIR}"
    RESULT_VARIABLE ILEMU_DYNARMIC_SHARED_RSB_CHECK
    OUTPUT_VARIABLE ILEMU_DYNARMIC_SHARED_RSB_OUTPUT
    ERROR_VARIABLE ILEMU_DYNARMIC_SHARED_RSB_ERROR)
if(ILEMU_DYNARMIC_SHARED_RSB_CHECK EQUAL 0)
    execute_process(
        COMMAND git apply "${ILEMU_DYNARMIC_SHARED_RSB_PATCH}"
        WORKING_DIRECTORY "${ILEMU_DYNARMIC_DIR}"
        RESULT_VARIABLE ILEMU_DYNARMIC_SHARED_RSB_APPLY
        OUTPUT_VARIABLE ILEMU_DYNARMIC_SHARED_RSB_APPLY_OUTPUT
        ERROR_VARIABLE ILEMU_DYNARMIC_SHARED_RSB_APPLY_ERROR)
    if(NOT ILEMU_DYNARMIC_SHARED_RSB_APPLY EQUAL 0)
        message(FATAL_ERROR
            "Failed to apply the Dynarmic shared RSB patch: "
            "${ILEMU_DYNARMIC_SHARED_RSB_APPLY_ERROR}")
    endif()
else()
    execute_process(
        COMMAND git apply --reverse --check
            "${ILEMU_DYNARMIC_SHARED_RSB_PATCH}"
        WORKING_DIRECTORY "${ILEMU_DYNARMIC_DIR}"
        RESULT_VARIABLE ILEMU_DYNARMIC_SHARED_RSB_REVERSE_CHECK
        OUTPUT_VARIABLE ILEMU_DYNARMIC_SHARED_RSB_REVERSE_OUTPUT
        ERROR_VARIABLE ILEMU_DYNARMIC_SHARED_RSB_REVERSE_ERROR)
    if(NOT ILEMU_DYNARMIC_SHARED_RSB_REVERSE_CHECK EQUAL 0)
        message(FATAL_ERROR
            "Dynarmic shared RSB patch is neither applicable nor already "
            "applied. git apply reported: "
            "${ILEMU_DYNARMIC_SHARED_RSB_ERROR}")
    endif()
endif()

# Publish shared-slab invalidation requests without forcing a generation
# transition on the caller's thread. Keep the ignored Dynarmic checkout
# reproducible and make reconfiguration idempotent when the patch is present.
set(ILEMU_DYNARMIC_SHARED_INVALIDATION_PATCH
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/patches/dynarmic-shared-invalidation.patch")
execute_process(
    COMMAND git apply --check "${ILEMU_DYNARMIC_SHARED_INVALIDATION_PATCH}"
    WORKING_DIRECTORY "${ILEMU_DYNARMIC_DIR}"
    RESULT_VARIABLE ILEMU_DYNARMIC_SHARED_INVALIDATION_CHECK
    OUTPUT_VARIABLE ILEMU_DYNARMIC_SHARED_INVALIDATION_OUTPUT
    ERROR_VARIABLE ILEMU_DYNARMIC_SHARED_INVALIDATION_ERROR)
if(ILEMU_DYNARMIC_SHARED_INVALIDATION_CHECK EQUAL 0)
    execute_process(
        COMMAND git apply "${ILEMU_DYNARMIC_SHARED_INVALIDATION_PATCH}"
        WORKING_DIRECTORY "${ILEMU_DYNARMIC_DIR}"
        RESULT_VARIABLE ILEMU_DYNARMIC_SHARED_INVALIDATION_APPLY
        OUTPUT_VARIABLE ILEMU_DYNARMIC_SHARED_INVALIDATION_APPLY_OUTPUT
        ERROR_VARIABLE ILEMU_DYNARMIC_SHARED_INVALIDATION_APPLY_ERROR)
    if(NOT ILEMU_DYNARMIC_SHARED_INVALIDATION_APPLY EQUAL 0)
        message(FATAL_ERROR
            "Failed to apply the Dynarmic shared invalidation patch: "
            "${ILEMU_DYNARMIC_SHARED_INVALIDATION_APPLY_ERROR}")
    endif()
else()
    execute_process(
        COMMAND git apply --reverse --check
            "${ILEMU_DYNARMIC_SHARED_INVALIDATION_PATCH}"
        WORKING_DIRECTORY "${ILEMU_DYNARMIC_DIR}"
        RESULT_VARIABLE ILEMU_DYNARMIC_SHARED_INVALIDATION_REVERSE_CHECK
        OUTPUT_VARIABLE ILEMU_DYNARMIC_SHARED_INVALIDATION_REVERSE_OUTPUT
        ERROR_VARIABLE ILEMU_DYNARMIC_SHARED_INVALIDATION_REVERSE_ERROR)
    if(NOT ILEMU_DYNARMIC_SHARED_INVALIDATION_REVERSE_CHECK EQUAL 0)
        # The range patch intentionally refines two shared-patch hunks. Once
        # it is applied, the older patch's reverse check can no longer match
        # those nearby lines; the range patch's reverse check proves that the
        # complete shared invalidation stack is already present.
        execute_process(
            COMMAND git apply --reverse --check
                "${CMAKE_CURRENT_SOURCE_DIR}/cmake/patches/dynarmic-shared-range-invalidation.patch"
            WORKING_DIRECTORY "${ILEMU_DYNARMIC_DIR}"
            RESULT_VARIABLE ILEMU_DYNARMIC_SHARED_RANGE_ALREADY_APPLIED
            OUTPUT_VARIABLE ILEMU_DYNARMIC_SHARED_RANGE_ALREADY_OUTPUT
            ERROR_VARIABLE ILEMU_DYNARMIC_SHARED_RANGE_ALREADY_ERROR)
        if(NOT ILEMU_DYNARMIC_SHARED_RANGE_ALREADY_APPLIED EQUAL 0)
            message(FATAL_ERROR
                "Dynarmic shared invalidation patch is neither applicable "
                "nor already applied. git apply reported: "
                "${ILEMU_DYNARMIC_SHARED_INVALIDATION_ERROR}")
        endif()
    endif()
endif()

# Retire only the guest blocks covered by a shared range invalidation. Keep
# this A32/x64 change as a tracked, idempotently applied patch as well.
set(ILEMU_DYNARMIC_SHARED_RANGE_INVALIDATION_PATCH
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/patches/dynarmic-shared-range-invalidation.patch")
execute_process(
    COMMAND git apply --check
        "${ILEMU_DYNARMIC_SHARED_RANGE_INVALIDATION_PATCH}"
    WORKING_DIRECTORY "${ILEMU_DYNARMIC_DIR}"
    RESULT_VARIABLE ILEMU_DYNARMIC_SHARED_RANGE_INVALIDATION_CHECK
    OUTPUT_VARIABLE ILEMU_DYNARMIC_SHARED_RANGE_INVALIDATION_OUTPUT
    ERROR_VARIABLE ILEMU_DYNARMIC_SHARED_RANGE_INVALIDATION_ERROR)
if(ILEMU_DYNARMIC_SHARED_RANGE_INVALIDATION_CHECK EQUAL 0)
    execute_process(
        COMMAND git apply
            "${ILEMU_DYNARMIC_SHARED_RANGE_INVALIDATION_PATCH}"
        WORKING_DIRECTORY "${ILEMU_DYNARMIC_DIR}"
        RESULT_VARIABLE ILEMU_DYNARMIC_SHARED_RANGE_INVALIDATION_APPLY
        OUTPUT_VARIABLE ILEMU_DYNARMIC_SHARED_RANGE_INVALIDATION_APPLY_OUTPUT
        ERROR_VARIABLE ILEMU_DYNARMIC_SHARED_RANGE_INVALIDATION_APPLY_ERROR)
    if(NOT ILEMU_DYNARMIC_SHARED_RANGE_INVALIDATION_APPLY EQUAL 0)
        message(FATAL_ERROR
            "Failed to apply the Dynarmic shared range invalidation patch: "
            "${ILEMU_DYNARMIC_SHARED_RANGE_INVALIDATION_APPLY_ERROR}")
    endif()
else()
    execute_process(
        COMMAND git apply --reverse --check
            "${ILEMU_DYNARMIC_SHARED_RANGE_INVALIDATION_PATCH}"
        WORKING_DIRECTORY "${ILEMU_DYNARMIC_DIR}"
        RESULT_VARIABLE ILEMU_DYNARMIC_SHARED_RANGE_INVALIDATION_REVERSE_CHECK
        OUTPUT_VARIABLE ILEMU_DYNARMIC_SHARED_RANGE_INVALIDATION_REVERSE_OUTPUT
        ERROR_VARIABLE ILEMU_DYNARMIC_SHARED_RANGE_INVALIDATION_REVERSE_ERROR)
    if(NOT ILEMU_DYNARMIC_SHARED_RANGE_INVALIDATION_REVERSE_CHECK EQUAL 0)
        message(FATAL_ERROR
            "Dynarmic shared range invalidation patch is neither applicable "
            "nor already applied. git apply reported: "
            "${ILEMU_DYNARMIC_SHARED_RANGE_INVALIDATION_ERROR}")
    endif()
endif()

# Persistent portable-IR artifacts must not cross a different Dynarmic build.
# Include the pinned revision and any applied working-tree patches so a
# locally modified dependency cannot silently reuse an older artifact key.
set(ILEMU_DYNARMIC_BUILD_FINGERPRINT 0x0ULL)
execute_process(
    COMMAND git rev-parse HEAD
    WORKING_DIRECTORY "${ILEMU_DYNARMIC_DIR}"
    RESULT_VARIABLE ILEMU_DYNARMIC_REVISION_RESULT
    OUTPUT_VARIABLE ILEMU_DYNARMIC_REVISION
    ERROR_QUIET
    OUTPUT_STRIP_TRAILING_WHITESPACE)
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
