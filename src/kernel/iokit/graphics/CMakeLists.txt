add_library(ilemu_kernel_iokit_graphics STATIC
    service.cpp)
add_library(iLEmu::kernel_iokit_graphics ALIAS
    ilemu_kernel_iokit_graphics)
ilemu_configure_module(ilemu_kernel_iokit_graphics)
target_link_libraries(ilemu_kernel_iokit_graphics PRIVATE
    iLEmu::foundation)
