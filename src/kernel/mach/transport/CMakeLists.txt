add_library(ilemu_kernel_mach_transport STATIC
    descriptor.cpp
    kernel_mach_ipc.cpp)
add_library(iLEmu::kernel_mach_transport ALIAS
    ilemu_kernel_mach_transport)
ilemu_configure_module(ilemu_kernel_mach_transport)
