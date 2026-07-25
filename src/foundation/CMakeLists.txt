add_subdirectory(memory)

add_library(ilemu_foundation STATIC
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
add_library(iLEmu::foundation ALIAS ilemu_foundation)
ilemu_configure_module(ilemu_foundation)
target_link_libraries(ilemu_foundation PUBLIC
    dynarmic Threads::Threads iLEmu::xnu_adapter
    iLEmu::foundation_memory)
