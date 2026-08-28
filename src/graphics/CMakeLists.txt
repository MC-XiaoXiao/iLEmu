add_library(ilemu_graphics STATIC
    core_animation_remote_profile.cpp
    core_surface_hle.cpp
    gles_renderer.cpp
    gles_primitive_assembler.cpp
    gles_resources.cpp
    gles_rasterizer.cpp
    gles_math.cpp
    host_graphics.cpp
    frame_file_presenter.cpp
    layerkit_compatibility.cpp
    layerkit_hle.cpp
    local_scene_background.cpp
    mbx_connect_hle.cpp
    mbx2d_hle.cpp
    mbx_client_surface.cpp
    mbx_swapchain.cpp
    mbx3d_quad_hle.cpp
    mobile_framebuffer_hle.cpp
    opengles_guest_profile.cpp
    gles_program_state.cpp
    opengles_hle.cpp
    opengles_programmable.cpp
    presentation_tracker.cpp
    scanout_composition.cpp
    sdl_display.cpp
    sdl_input.cpp
    surface_store.cpp)
target_sources(ilemu_graphics PRIVATE touch_replay.cpp)
find_package(PNG REQUIRED)
if(ILEMU_ENABLE_VULKAN)
    find_package(Vulkan QUIET)
endif()
add_library(iLEmu::graphics ALIAS ilemu_graphics)
ilemu_configure_module(ilemu_graphics)
target_link_libraries(ilemu_graphics
    PUBLIC iLEmu::foundation
    PRIVATE PNG::PNG)
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
            "${CMAKE_CURRENT_SOURCE_DIR}/host/shaders/gles.vert"
        COMMAND "${Vulkan_GLSLC_EXECUTABLE}" -O -mfmt=c
            -fshader-stage=frag
            -o "${ILEMU_VULKAN_FRAGMENT_SHADER}"
            "${CMAKE_CURRENT_SOURCE_DIR}/host/shaders/gles.frag"
        DEPENDS
            host/shaders/gles.vert
            host/shaders/gles.frag
        VERBATIM)
    add_custom_target(ilemu_vulkan_shaders DEPENDS
        "${ILEMU_VULKAN_VERTEX_SHADER}"
        "${ILEMU_VULKAN_FRAGMENT_SHADER}")
    target_sources(ilemu_graphics PRIVATE
        host/vulkan_gles_renderer.cpp
        host/vulkan_memory_allocator.cpp)
    add_dependencies(ilemu_graphics ilemu_vulkan_shaders)
    target_include_directories(ilemu_graphics PRIVATE
        "${ILEMU_VULKAN_SHADER_DIR}")
    target_include_directories(ilemu_graphics SYSTEM PRIVATE
        "${ILEMU_VMA_DIR}/include")
    target_link_libraries(ilemu_graphics PRIVATE Vulkan::Vulkan)
    target_compile_definitions(ilemu_graphics PRIVATE
        ILEMU_HAS_VULKAN=1)
endif()
if(TARGET PkgConfig::SDL2)
    target_link_libraries(ilemu_graphics PUBLIC PkgConfig::SDL2)
    target_compile_definitions(ilemu_graphics PUBLIC
        ILEMU_HAS_SDL2=1)
endif()
