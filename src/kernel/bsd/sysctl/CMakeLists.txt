add_library(ilemu_kernel_bsd_sysctl STATIC
    hardware_values.cpp
    name_resolver.cpp
    process_arguments.cpp)
add_library(iLEmu::kernel_bsd_sysctl ALIAS
    ilemu_kernel_bsd_sysctl)
ilemu_configure_module(ilemu_kernel_bsd_sysctl)
