add_library(ilemu_kernel_mach_task STATIC
    identity.cpp
    info.cpp
    enumeration.cpp
    exception_ports.cpp)
add_library(iLEmu::kernel_mach_task ALIAS
    ilemu_kernel_mach_task)
ilemu_configure_module(ilemu_kernel_mach_task)
target_link_libraries(ilemu_kernel_mach_task PRIVATE
    iLEmu::foundation
    iLEmu::mach)
