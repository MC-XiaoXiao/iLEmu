add_library(ilemu_graphics STATIC
    core_surface_hle.cpp
    gles_renderer.cpp
    gles_resources.cpp
    gles_rasterizer.cpp
    gles_math.cpp
    frame_file_presenter.cpp
    layerkit_compatibility.cpp
    layerkit_hle.cpp
    mbx_connect_hle.cpp
    mbx2d_hle.cpp
    mbx_client_surface.cpp
    mbx_swapchain.cpp
    mbx3d_quad_hle.cpp
    mobile_framebuffer_hle.cpp
    opengles_hle.cpp
    presentation_tracker.cpp
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
if(Vulkan_FOUND AND PkgConfig_FOUND)
    pkg_check_modules(SHADERC QUIET IMPORTED_TARGET shaderc)
endif()
if(Vulkan_FOUND AND TARGET PkgConfig::SHADERC)
    target_sources(ilemu_graphics PRIVATE
        host/vulkan_gles_renderer.cpp)
    target_link_libraries(ilemu_graphics PRIVATE
        Vulkan::Vulkan PkgConfig::SHADERC)
    target_compile_definitions(ilemu_graphics PRIVATE
        ILEMU_HAS_VULKAN=1)
endif()
if(TARGET PkgConfig::SDL2)
    target_link_libraries(ilemu_graphics PUBLIC PkgConfig::SDL2)
    target_compile_definitions(ilemu_graphics PUBLIC
        ILEMU_HAS_SDL2=1)
endif()
