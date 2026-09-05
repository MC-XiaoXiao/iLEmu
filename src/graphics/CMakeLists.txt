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
    surface_store.cpp)
target_sources(ilemu_graphics PRIVATE touch_replay.cpp)
find_package(PNG REQUIRED)
add_library(iLEmu::graphics ALIAS ilemu_graphics)
ilemu_configure_module(ilemu_graphics)
target_link_libraries(ilemu_graphics
    PUBLIC iLEmu::foundation
    PRIVATE PNG::PNG)
