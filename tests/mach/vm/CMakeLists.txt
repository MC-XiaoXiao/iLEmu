add_library(ilemu_kernel_mach_vm_tests STATIC
    allocate_tests.cpp
    copy_tests.cpp
    main.cpp
    memory_entry_tests.cpp
    purgable_tests.cpp
    read_tests.cpp)
add_library(iLEmu::kernel_mach_vm_tests ALIAS
    ilemu_kernel_mach_vm_tests)
ilemu_configure_module(ilemu_kernel_mach_vm_tests)
target_link_libraries(ilemu_kernel_mach_vm_tests PRIVATE
    iLEmu::core
    ilemu_test_support)
