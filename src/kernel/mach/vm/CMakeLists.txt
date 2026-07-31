add_library(ilemu_kernel_mach_vm STATIC
    allocate.cpp
    copy.cpp
    deallocate.cpp
    map.cpp
    memory_entry.cpp
    protect.cpp
    purgable.cpp
    read.cpp
    region.cpp)
add_library(iLEmu::kernel_mach_vm ALIAS
    ilemu_kernel_mach_vm)
ilemu_configure_module(ilemu_kernel_mach_vm)
target_link_libraries(ilemu_kernel_mach_vm PRIVATE
    iLEmu::foundation)
