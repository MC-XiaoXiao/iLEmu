add_library(ilemu_kernel_mach_thread STATIC
    lifecycle.cpp
    self.cpp
    state.cpp)
add_library(iLEmu::kernel_mach_thread ALIAS
    ilemu_kernel_mach_thread)
ilemu_configure_module(ilemu_kernel_mach_thread)
target_link_libraries(ilemu_kernel_mach_thread PRIVATE
    iLEmu::foundation
    iLEmu::mach)
