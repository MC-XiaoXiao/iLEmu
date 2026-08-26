add_library(ilemu_kernel_iokit_keybag STATIC
    service.cpp)
add_library(iLEmu::kernel_iokit_keybag ALIAS
    ilemu_kernel_iokit_keybag)
ilemu_configure_module(ilemu_kernel_iokit_keybag)
target_link_libraries(ilemu_kernel_iokit_keybag PRIVATE
    iLEmu::foundation)
