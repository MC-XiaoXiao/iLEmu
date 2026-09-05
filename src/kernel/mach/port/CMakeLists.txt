add_library(ilemu_kernel_mach_port STATIC
    context.cpp
    enumeration.cpp
    kernel_rpc.cpp
    limits.cpp
    membership.cpp)
add_library(iLEmu::kernel_mach_port ALIAS
    ilemu_kernel_mach_port)
ilemu_configure_module(ilemu_kernel_mach_port)
target_link_libraries(ilemu_kernel_mach_port PRIVATE
    iLEmu::foundation)
