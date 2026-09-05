add_library(ilemu_host_audio STATIC
    audio/ffmpeg_audio_decoder.cpp)
add_library(iLEmu::host_audio ALIAS ilemu_host_audio)
ilemu_configure_module(ilemu_host_audio)
target_link_libraries(ilemu_host_audio PUBLIC iLEmu::media)
if(TARGET PkgConfig::FFMPEG)
    target_link_libraries(ilemu_host_audio PRIVATE PkgConfig::FFMPEG)
    target_compile_definitions(ilemu_host_audio PRIVATE
        ILEMU_HAS_FFMPEG=1)
endif()

add_library(ilemu_host_graphics STATIC
    graphics/native_gles.cpp)
add_library(iLEmu::host_graphics ALIAS ilemu_host_graphics)
ilemu_configure_module(ilemu_host_graphics)
target_link_libraries(ilemu_host_graphics PUBLIC iLEmu::graphics)
if(ILEMU_ENABLE_VULKAN)
    find_package(Vulkan QUIET)
endif()
if(Vulkan_FOUND AND Vulkan_GLSLC_EXECUTABLE)
    set(ILEMU_VMA_DIR
        "${PROJECT_SOURCE_DIR}/external/vulkan-memory-allocator")
    if(NOT EXISTS "${ILEMU_VMA_DIR}/include/vk_mem_alloc.h")
        message(FATAL_ERROR
            "Vulkan Memory Allocator was not found at ${ILEMU_VMA_DIR}.")
    endif()
    set(ILEMU_VULKAN_SHADER_DIR
        "${CMAKE_CURRENT_BINARY_DIR}/graphics/generated/vulkan")
    set(ILEMU_VULKAN_VERTEX_SHADER
        "${ILEMU_VULKAN_SHADER_DIR}/gles.vert.inc")
    set(ILEMU_VULKAN_FRAGMENT_SHADER
        "${ILEMU_VULKAN_SHADER_DIR}/gles.frag.inc")
    add_custom_command(
        OUTPUT
            "${ILEMU_VULKAN_VERTEX_SHADER}"
            "${ILEMU_VULKAN_FRAGMENT_SHADER}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory
            "${ILEMU_VULKAN_SHADER_DIR}"
        COMMAND "${Vulkan_GLSLC_EXECUTABLE}" -O -mfmt=c
            -fshader-stage=vert
            -o "${ILEMU_VULKAN_VERTEX_SHADER}"
            "${CMAKE_CURRENT_SOURCE_DIR}/graphics/shaders/gles.vert"
        COMMAND "${Vulkan_GLSLC_EXECUTABLE}" -O -mfmt=c
            -fshader-stage=frag
            -o "${ILEMU_VULKAN_FRAGMENT_SHADER}"
            "${CMAKE_CURRENT_SOURCE_DIR}/graphics/shaders/gles.frag"
        DEPENDS
            graphics/shaders/gles.vert
            graphics/shaders/gles.frag
        VERBATIM)
    add_custom_target(ilemu_vulkan_shaders DEPENDS
        "${ILEMU_VULKAN_VERTEX_SHADER}"
        "${ILEMU_VULKAN_FRAGMENT_SHADER}")
    target_sources(ilemu_host_graphics PRIVATE
        graphics/vulkan_gles_renderer.cpp
        graphics/vulkan_memory_allocator.cpp)
    add_dependencies(ilemu_host_graphics ilemu_vulkan_shaders)
    target_include_directories(ilemu_host_graphics PRIVATE
        "${ILEMU_VULKAN_SHADER_DIR}")
    target_include_directories(ilemu_host_graphics SYSTEM PRIVATE
        "${ILEMU_VMA_DIR}/include")
    target_link_libraries(ilemu_host_graphics PRIVATE Vulkan::Vulkan)
    target_compile_definitions(ilemu_host_graphics PRIVATE
        ILEMU_HAS_VULKAN=1)
endif()

add_library(ilemu_host_system STATIC system/resource_usage.cpp)
add_library(iLEmu::host_system ALIAS ilemu_host_system)
ilemu_configure_module(ilemu_host_system)
target_link_libraries(ilemu_host_system PUBLIC iLEmu::foundation)

add_library(ilemu_host INTERFACE)
add_library(iLEmu::host ALIAS ilemu_host)
target_link_libraries(ilemu_host INTERFACE
    iLEmu::host_audio
    iLEmu::host_graphics
    iLEmu::host_system)
