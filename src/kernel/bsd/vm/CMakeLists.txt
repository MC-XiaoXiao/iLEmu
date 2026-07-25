add_library(ilemu_kernel_bsd_vm_shared_region STATIC
    shared_region.cpp)
add_library(iLEmu::kernel_bsd_vm_shared_region ALIAS
    ilemu_kernel_bsd_vm_shared_region)
ilemu_configure_module(ilemu_kernel_bsd_vm_shared_region)
target_link_libraries(ilemu_kernel_bsd_vm_shared_region PRIVATE
    iLEmu::foundation)
