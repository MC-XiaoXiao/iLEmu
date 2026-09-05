add_library(ilemu_xnu_adapter STATIC adapter/xnu_mig_adapter.cpp)
add_library(iLEmu::xnu_adapter ALIAS ilemu_xnu_adapter)
ilemu_configure_module(ilemu_xnu_adapter)

add_library(ilemu_mach STATIC
    guest_dispatch_policy.cpp
    guest_execution_policy.cpp
    guest_parallelism_policy.cpp
    mach_namespace.cpp
    xnu_scheduler.cpp)
add_library(iLEmu::mach ALIAS ilemu_mach)
ilemu_configure_module(ilemu_mach)
target_link_libraries(ilemu_mach PUBLIC
    iLEmu::foundation iLEmu::xnu_adapter)
