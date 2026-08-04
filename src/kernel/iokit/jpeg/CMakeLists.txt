add_library(ilemu_kernel_iokit_jpeg STATIC
    service.cpp)
add_library(iLEmu::kernel_iokit_jpeg ALIAS
    ilemu_kernel_iokit_jpeg)
ilemu_configure_module(ilemu_kernel_iokit_jpeg)
target_link_libraries(ilemu_kernel_iokit_jpeg PRIVATE
    iLEmu::foundation
    iLEmu::graphics
    iLEmu::media)
