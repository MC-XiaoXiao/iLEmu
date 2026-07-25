add_subdirectory(memory)

add_library(ilegacysim_foundation STATIC
    address_space.cpp
    arm_cpu_model.cpp
    cpu.cpp
    darwin_notify_state_hle.cpp
    device_profile.cpp
    display.cpp
    macho.cpp
    output.cpp
    process_loader.cpp
    scene_coordinator.cpp
    userland_hle.cpp
    virtual_clock.cpp)
add_library(iLegacySim::foundation ALIAS ilegacysim_foundation)
ilegacysim_configure_module(ilegacysim_foundation)
target_link_libraries(ilegacysim_foundation PUBLIC
    dynarmic Threads::Threads iLegacySim::xnu_adapter
    iLegacySim::foundation_memory)
