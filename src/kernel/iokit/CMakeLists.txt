add_subdirectory(baseband)
add_subdirectory(display)

add_library(ilemu_kernel_iokit_power STATIC
    power.cpp)
add_library(iLEmu::kernel_iokit_power ALIAS
    ilemu_kernel_iokit_power)
ilemu_configure_module(ilemu_kernel_iokit_power)
target_link_libraries(ilemu_kernel_iokit_power PRIVATE
    iLEmu::foundation)
