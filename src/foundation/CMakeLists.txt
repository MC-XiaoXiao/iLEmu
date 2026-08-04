add_subdirectory(memory)

add_library(ilemu_foundation STATIC
    address_space.cpp
    arm_cpu_model.cpp
    cpu.cpp
    darwin_notify_state_hle.cpp
    device_profile.cpp
    display.cpp
    jit_translation_profile.cpp
    macho.cpp
    output.cpp
    performance.cpp
    process_loader.cpp
    rootfs_path_resolver.cpp
    scene_coordinator.cpp
    userland_hle.cpp
    virtual_clock.cpp)
add_library(iLEmu::foundation ALIAS ilemu_foundation)
ilemu_configure_module(ilemu_foundation)
target_include_directories(ilemu_foundation SYSTEM PRIVATE
    "${ILEMU_DYNARMIC_DIR}/externals/mcl/include"
    "${ILEMU_BOOST_DIR}")
target_link_libraries(ilemu_foundation PUBLIC
    dynarmic Threads::Threads iLEmu::xnu_adapter
    iLEmu::foundation_memory)
