add_library(ilemu_kernel_mach_input STATIC
    graphics_services.cpp)
add_library(iLEmu::kernel_mach_input ALIAS ilemu_kernel_mach_input)
ilemu_configure_module(ilemu_kernel_mach_input)
target_link_libraries(ilemu_kernel_mach_input PUBLIC
    iLEmu::foundation
    iLEmu::mach)
