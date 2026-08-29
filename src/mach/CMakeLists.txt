add_library(ilemu_mach STATIC
    guest_execution_policy.cpp
    guest_parallelism_policy.cpp
    mach_namespace.cpp
    kernel_mach_ipc.cpp
    xnu_scheduler.cpp)
add_library(iLEmu::mach ALIAS ilemu_mach)
ilemu_configure_module(ilemu_mach)
target_link_libraries(ilemu_mach PUBLIC
    iLEmu::foundation iLEmu::xnu_adapter)
