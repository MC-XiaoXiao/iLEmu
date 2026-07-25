add_library(ilemu_kernel_mach_processor STATIC
    processor_set.cpp)
add_library(iLEmu::kernel_mach_processor ALIAS
    ilemu_kernel_mach_processor)
ilemu_configure_module(ilemu_kernel_mach_processor)
target_link_libraries(ilemu_kernel_mach_processor PRIVATE
    iLEmu::foundation
    iLEmu::mach)
