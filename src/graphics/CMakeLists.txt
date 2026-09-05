add_library(ilemu_graphics STATIC
    display.cpp
    core_animation_remote_abi.cpp
    gles_renderer.cpp
    gles_primitive_assembler.cpp
    gles_resources.cpp
    gles_rasterizer.cpp
    gles_math.cpp
    host_graphics.cpp
    frame_file_presenter.cpp
    layerkit_compatibility.cpp
    local_scene_background.cpp
    opengles_guest_capabilities.cpp
    gles_program_state.cpp
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
