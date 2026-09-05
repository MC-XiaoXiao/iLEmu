add_library(ilemu_host_graphics STATIC
    native_gles.cpp)
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
        "${CMAKE_CURRENT_BINARY_DIR}/generated/vulkan")
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
            "${CMAKE_CURRENT_SOURCE_DIR}/shaders/gles.vert"
        COMMAND "${Vulkan_GLSLC_EXECUTABLE}" -O -mfmt=c
            -fshader-stage=frag
            -o "${ILEMU_VULKAN_FRAGMENT_SHADER}"
            "${CMAKE_CURRENT_SOURCE_DIR}/shaders/gles.frag"
        DEPENDS
            shaders/gles.vert
            shaders/gles.frag
        VERBATIM)
    add_custom_target(ilemu_vulkan_shaders DEPENDS
        "${ILEMU_VULKAN_VERTEX_SHADER}"
        "${ILEMU_VULKAN_FRAGMENT_SHADER}")
    target_sources(ilemu_host_graphics PRIVATE
        vulkan_gles_renderer.cpp
        vulkan_memory_allocator.cpp)
    add_dependencies(ilemu_host_graphics ilemu_vulkan_shaders)
    target_include_directories(ilemu_host_graphics PRIVATE
        "${ILEMU_VULKAN_SHADER_DIR}")
    target_include_directories(ilemu_host_graphics SYSTEM PRIVATE
        "${ILEMU_VMA_DIR}/include")
    target_link_libraries(ilemu_host_graphics PRIVATE Vulkan::Vulkan)
    target_compile_definitions(ilemu_host_graphics PRIVATE
        ILEMU_HAS_VULKAN=1)
endif()
