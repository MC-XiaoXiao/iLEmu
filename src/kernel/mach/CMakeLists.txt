add_subdirectory(transport)
add_subdirectory(port)
add_subdirectory(processor)
add_subdirectory(task)
add_subdirectory(thread)
add_subdirectory(vm)
add_subdirectory(input)

target_sources(ilemu_kernel PRIVATE
    support.cpp
    ipc.cpp
    semaphore.cpp
    message.cpp
    host_message.cpp
    port_message.cpp
    task_vm_message.cpp
    rights_message.cpp
    notification_message.cpp
    traps.cpp)
target_link_libraries(ilemu_kernel PRIVATE
    iLEmu::kernel_mach_transport
    iLEmu::kernel_mach_port
    iLEmu::kernel_mach_processor
    iLEmu::kernel_mach_task
    iLEmu::kernel_mach_thread
    iLEmu::kernel_mach_vm
    iLEmu::kernel_mach_input)
