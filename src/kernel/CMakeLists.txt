add_library(ilemu_kernel_api INTERFACE)
add_library(iLEmu::kernel_api ALIAS ilemu_kernel_api)
target_include_directories(ilemu_kernel_api INTERFACE
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>")
target_link_libraries(ilemu_kernel_api INTERFACE
    iLEmu::device_state
    iLEmu::foundation
    iLEmu::mach
    iLEmu::filesystem
    iLEmu::network
    iLEmu::storage
    iLEmu::telephony
    iLEmu::media
    iLEmu::bluetooth
    iLEmu::crypto
    iLEmu::graphics
    iLEmu::xnu_adapter)

add_library(ilemu_kernel STATIC
    kernel.cpp
    hid_event_system_hle.cpp
    clock.cpp
    iokit.cpp
    vnode_watch.cpp
    network.cpp)
add_library(iLEmu::kernel ALIAS ilemu_kernel)
ilemu_configure_module(ilemu_kernel)

add_library(ilemu_kernel_graphics STATIC
    graphics/core_surface_hle.cpp
    graphics/layerkit_hle.cpp
    graphics/mbx2d_hle.cpp
    graphics/mbx3d_quad_hle.cpp
    graphics/mbx_client_surface.cpp
    graphics/mbx_swapchain.cpp
    graphics/mobile_framebuffer_hle.cpp
    graphics/opengles_hle.cpp
    graphics/opengles_dispatch_hle.cpp
    graphics/core_animation_software_hle.cpp
    graphics/opengles_programmable.cpp
    graphics/mbx_connect_hle.cpp)
add_library(iLEmu::kernel_graphics ALIAS ilemu_kernel_graphics)
ilemu_configure_module(ilemu_kernel_graphics)

add_library(ilemu_kernel_bsd_baseband_device STATIC
    bsd/device/baseband/device.cpp
    bsd/device/baseband/offline_control.cpp
    bsd/device/baseband/replay.cpp)
add_library(iLEmu::kernel_bsd_baseband_device ALIAS
    ilemu_kernel_bsd_baseband_device)
ilemu_configure_module(ilemu_kernel_bsd_baseband_device)
target_link_libraries(ilemu_kernel_bsd_baseband_device PRIVATE
    iLEmu::foundation)

add_library(ilemu_kernel_bsd_debug STATIC
    bsd/debug/tracing.cpp)
add_library(iLEmu::kernel_bsd_debug ALIAS
    ilemu_kernel_bsd_debug)
ilemu_configure_module(ilemu_kernel_bsd_debug)
target_link_libraries(ilemu_kernel_bsd_debug PRIVATE
    iLEmu::foundation)

add_library(ilemu_kernel_bsd_sysctl STATIC
    bsd/sysctl/hardware_values.cpp
    bsd/sysctl/name_resolver.cpp
    bsd/sysctl/process_arguments.cpp)
add_library(iLEmu::kernel_bsd_sysctl ALIAS
    ilemu_kernel_bsd_sysctl)
ilemu_configure_module(ilemu_kernel_bsd_sysctl)

add_library(ilemu_kernel_bsd_signal STATIC
    bsd/signal/signal.cpp)
add_library(iLEmu::kernel_bsd_signal ALIAS
    ilemu_kernel_bsd_signal)
ilemu_configure_module(ilemu_kernel_bsd_signal)
target_link_libraries(ilemu_kernel_bsd_signal PRIVATE
    iLEmu::foundation)

add_library(ilemu_kernel_bsd_filesystem_control STATIC
    bsd/filesystem/control.cpp)
add_library(iLEmu::kernel_bsd_filesystem_control ALIAS
    ilemu_kernel_bsd_filesystem_control)
ilemu_configure_module(ilemu_kernel_bsd_filesystem_control)
target_link_libraries(ilemu_kernel_bsd_filesystem_control PRIVATE
    iLEmu::filesystem
    iLEmu::foundation)

add_library(ilemu_kernel_bsd_filesystem_ownership STATIC
    bsd/filesystem/ownership.cpp)
add_library(iLEmu::kernel_bsd_filesystem_ownership ALIAS
    ilemu_kernel_bsd_filesystem_ownership)
ilemu_configure_module(ilemu_kernel_bsd_filesystem_ownership)
target_link_libraries(ilemu_kernel_bsd_filesystem_ownership PRIVATE
    iLEmu::filesystem
    iLEmu::foundation)

add_library(ilemu_kernel_bsd_filesystem_locking STATIC
    bsd/filesystem/locking.cpp)
add_library(iLEmu::kernel_bsd_filesystem_locking ALIAS
    ilemu_kernel_bsd_filesystem_locking)
ilemu_configure_module(ilemu_kernel_bsd_filesystem_locking)
target_link_libraries(ilemu_kernel_bsd_filesystem_locking PRIVATE
    iLEmu::filesystem
    iLEmu::foundation)

add_library(ilemu_kernel_bsd_filesystem_persistence STATIC
    bsd/filesystem/persistence.cpp)
add_library(iLEmu::kernel_bsd_filesystem_persistence ALIAS
    ilemu_kernel_bsd_filesystem_persistence)
ilemu_configure_module(ilemu_kernel_bsd_filesystem_persistence)
target_link_libraries(ilemu_kernel_bsd_filesystem_persistence PRIVATE
    iLEmu::filesystem
    iLEmu::foundation)

add_library(ilemu_kernel_bsd_filesystem_timestamps STATIC
    bsd/filesystem/timestamps.cpp)
add_library(iLEmu::kernel_bsd_filesystem_timestamps ALIAS
    ilemu_kernel_bsd_filesystem_timestamps)
ilemu_configure_module(ilemu_kernel_bsd_filesystem_timestamps)
target_link_libraries(ilemu_kernel_bsd_filesystem_timestamps PRIVATE
    iLEmu::filesystem
    iLEmu::foundation)

add_library(ilemu_kernel_bsd_control STATIC
    bsd/network/control/control.cpp
    bsd/network/control/syscalls.cpp)
add_library(iLEmu::kernel_bsd_control ALIAS
    ilemu_kernel_bsd_control)
ilemu_configure_module(ilemu_kernel_bsd_control)
target_link_libraries(ilemu_kernel_bsd_control PRIVATE
    iLEmu::foundation)

add_library(ilemu_kernel_bsd_virtual_udp STATIC
    bsd/network/virtual_udp.cpp)
add_library(iLEmu::kernel_bsd_virtual_udp ALIAS
    ilemu_kernel_bsd_virtual_udp)
ilemu_configure_module(ilemu_kernel_bsd_virtual_udp)
target_link_libraries(ilemu_kernel_bsd_virtual_udp PRIVATE
    iLEmu::foundation)

add_library(ilemu_kernel_bsd_virtual_udp_syscalls STATIC
    bsd/network/virtual_udp_syscalls.cpp)
add_library(iLEmu::kernel_bsd_virtual_udp_syscalls ALIAS
    ilemu_kernel_bsd_virtual_udp_syscalls)
ilemu_configure_module(ilemu_kernel_bsd_virtual_udp_syscalls)
target_link_libraries(ilemu_kernel_bsd_virtual_udp_syscalls PRIVATE
    iLEmu::kernel_bsd_virtual_udp
    iLEmu::foundation)

add_library(ilemu_kernel_bsd_process STATIC
    bsd/process/credentials.cpp
    bsd/process/diagnostics.cpp
    bsd/process/information.cpp
    bsd/process/interval_timer.cpp
    bsd/process/spawn.cpp)
add_library(iLEmu::kernel_bsd_process ALIAS
    ilemu_kernel_bsd_process)
ilemu_configure_module(ilemu_kernel_bsd_process)
target_link_libraries(ilemu_kernel_bsd_process PRIVATE
    iLEmu::foundation)

add_library(ilemu_kernel_bsd_security STATIC
    bsd/security/mac.cpp
    bsd/security/sandbox.cpp)
add_library(iLEmu::kernel_bsd_security ALIAS
    ilemu_kernel_bsd_security)
ilemu_configure_module(ilemu_kernel_bsd_security)
target_link_libraries(ilemu_kernel_bsd_security PRIVATE
    iLEmu::foundation)

add_library(ilemu_kernel_bsd_vm_shared_region STATIC
    bsd/vm/shared_region.cpp)
add_library(iLEmu::kernel_bsd_vm_shared_region ALIAS
    ilemu_kernel_bsd_vm_shared_region)
ilemu_configure_module(ilemu_kernel_bsd_vm_shared_region)
target_link_libraries(ilemu_kernel_bsd_vm_shared_region PRIVATE
    iLEmu::foundation)

target_sources(ilemu_kernel PRIVATE
    bsd/dispatch.cpp
    bsd/platform.cpp
    bsd/pthread_runtime.cpp
    bsd/psynch_runtime.cpp
    bsd/psynch.cpp
    bsd/aio.cpp
    bsd/audit_session.cpp
    bsd/fileport.cpp
    bsd/process.cpp
    bsd/posix_semaphore.cpp
    bsd/filesystem.cpp
    bsd/descriptor_lifecycle.cpp
    bsd/descriptor_memory.cpp
    bsd/bpf.cpp
    bsd/socket.cpp
    bsd/network/process_sockets.cpp
    bsd/events.cpp
    bsd/device/offline_serial.cpp
    bsd/support.cpp)
target_link_libraries(ilemu_kernel PRIVATE
    iLEmu::kernel_bsd_baseband_device
    iLEmu::kernel_bsd_control
    iLEmu::kernel_bsd_debug
    iLEmu::kernel_bsd_filesystem_control
    iLEmu::kernel_bsd_filesystem_persistence
    iLEmu::kernel_bsd_filesystem_timestamps
    iLEmu::kernel_bsd_filesystem_locking
    iLEmu::kernel_bsd_filesystem_ownership
    iLEmu::kernel_bsd_virtual_udp_syscalls
    iLEmu::kernel_bsd_virtual_udp
    iLEmu::kernel_bsd_process
    iLEmu::kernel_bsd_security
    iLEmu::kernel_bsd_vm_shared_region
    iLEmu::kernel_bsd_signal
    iLEmu::kernel_bsd_sysctl)

add_library(ilemu_kernel_iokit_audio STATIC
    iokit/audio/device_catalog.cpp
    iokit/audio/abi.cpp
    iokit/audio/service.cpp)
add_library(iLEmu::kernel_iokit_audio ALIAS
    ilemu_kernel_iokit_audio)
ilemu_configure_module(ilemu_kernel_iokit_audio)
target_link_libraries(ilemu_kernel_iokit_audio PRIVATE
    iLEmu::foundation)

add_library(ilemu_kernel_iokit_baseband STATIC
    iokit/baseband/service.cpp)
add_library(iLEmu::kernel_iokit_baseband ALIAS
    ilemu_kernel_iokit_baseband)
ilemu_configure_module(ilemu_kernel_iokit_baseband)
target_link_libraries(ilemu_kernel_iokit_baseband PRIVATE
    iLEmu::foundation)

add_library(ilemu_kernel_iokit_camera STATIC
    iokit/camera/service.cpp)
add_library(iLEmu::kernel_iokit_camera ALIAS
    ilemu_kernel_iokit_camera)
ilemu_configure_module(ilemu_kernel_iokit_camera)
target_link_libraries(ilemu_kernel_iokit_camera PRIVATE
    iLEmu::foundation)

add_library(ilemu_kernel_iokit_display STATIC
    iokit/display/vsync.cpp)
add_library(iLEmu::kernel_iokit_display ALIAS
    ilemu_kernel_iokit_display)
ilemu_configure_module(ilemu_kernel_iokit_display)
target_link_libraries(ilemu_kernel_iokit_display PRIVATE
    iLEmu::foundation)

add_library(ilemu_kernel_iokit_graphics STATIC
    iokit/graphics/service.cpp)
add_library(iLEmu::kernel_iokit_graphics ALIAS
    ilemu_kernel_iokit_graphics)
ilemu_configure_module(ilemu_kernel_iokit_graphics)
target_link_libraries(ilemu_kernel_iokit_graphics PRIVATE
    iLEmu::foundation)

add_library(ilemu_kernel_iokit_hid STATIC
    iokit/hid/service.cpp)
add_library(iLEmu::kernel_iokit_hid ALIAS
    ilemu_kernel_iokit_hid)
ilemu_configure_module(ilemu_kernel_iokit_hid)
target_link_libraries(ilemu_kernel_iokit_hid PRIVATE
    iLEmu::foundation)

add_library(ilemu_kernel_iokit_jpeg STATIC
    iokit/jpeg/service.cpp)
add_library(iLEmu::kernel_iokit_jpeg ALIAS
    ilemu_kernel_iokit_jpeg)
ilemu_configure_module(ilemu_kernel_iokit_jpeg)
target_link_libraries(ilemu_kernel_iokit_jpeg PRIVATE
    iLEmu::foundation
    iLEmu::graphics
    iLEmu::media)

add_library(ilemu_kernel_iokit_keybag STATIC
    iokit/keybag/service.cpp)
add_library(iLEmu::kernel_iokit_keybag ALIAS
    ilemu_kernel_iokit_keybag)
ilemu_configure_module(ilemu_kernel_iokit_keybag)
target_link_libraries(ilemu_kernel_iokit_keybag PRIVATE
    iLEmu::foundation iLEmu::crypto)

add_library(ilemu_kernel_iokit_mbx STATIC
    iokit/mbx/service.cpp)
add_library(iLEmu::kernel_iokit_mbx ALIAS
    ilemu_kernel_iokit_mbx)
ilemu_configure_module(ilemu_kernel_iokit_mbx)
target_link_libraries(ilemu_kernel_iokit_mbx PRIVATE
    iLEmu::foundation)

add_library(ilemu_kernel_iokit_mobile_file_integrity STATIC
    iokit/mobile_file_integrity/service.cpp)
add_library(iLEmu::kernel_iokit_mobile_file_integrity ALIAS
    ilemu_kernel_iokit_mobile_file_integrity)
ilemu_configure_module(ilemu_kernel_iokit_mobile_file_integrity)
target_link_libraries(ilemu_kernel_iokit_mobile_file_integrity PRIVATE
    iLEmu::foundation)

add_library(ilemu_kernel_iokit_power STATIC
    iokit/battery.cpp
    iokit/power.cpp)
add_library(iLEmu::kernel_iokit_power ALIAS
    ilemu_kernel_iokit_power)
ilemu_configure_module(ilemu_kernel_iokit_power)
target_link_libraries(ilemu_kernel_iokit_power PRIVATE
    iLEmu::foundation)

add_library(ilemu_kernel_mach_transport STATIC
    mach/transport/descriptor.cpp
    mach/transport/kernel_mach_ipc.cpp)
add_library(iLEmu::kernel_mach_transport ALIAS
    ilemu_kernel_mach_transport)
ilemu_configure_module(ilemu_kernel_mach_transport)

add_library(ilemu_kernel_mach_port STATIC
    mach/port/context.cpp
    mach/port/enumeration.cpp
    mach/port/kernel_rpc.cpp
    mach/port/limits.cpp
    mach/port/membership.cpp)
add_library(iLEmu::kernel_mach_port ALIAS
    ilemu_kernel_mach_port)
ilemu_configure_module(ilemu_kernel_mach_port)
target_link_libraries(ilemu_kernel_mach_port PRIVATE
    iLEmu::foundation)

add_library(ilemu_kernel_mach_processor STATIC
    mach/processor/processor_set.cpp)
add_library(iLEmu::kernel_mach_processor ALIAS
    ilemu_kernel_mach_processor)
ilemu_configure_module(ilemu_kernel_mach_processor)
target_link_libraries(ilemu_kernel_mach_processor PRIVATE
    iLEmu::foundation
    iLEmu::mach)

add_library(ilemu_kernel_mach_task STATIC
    mach/task/identity.cpp
    mach/task/info.cpp
    mach/task/enumeration.cpp
    mach/task/exception_ports.cpp)
add_library(iLEmu::kernel_mach_task ALIAS
    ilemu_kernel_mach_task)
ilemu_configure_module(ilemu_kernel_mach_task)
target_link_libraries(ilemu_kernel_mach_task PRIVATE
    iLEmu::foundation
    iLEmu::mach)

add_library(ilemu_kernel_mach_thread STATIC
    mach/thread/creation.cpp
    mach/thread/lifecycle.cpp
    mach/thread/self.cpp
    mach/thread/state.cpp)
add_library(iLEmu::kernel_mach_thread ALIAS
    ilemu_kernel_mach_thread)
ilemu_configure_module(ilemu_kernel_mach_thread)
target_link_libraries(ilemu_kernel_mach_thread PRIVATE
    iLEmu::foundation
    iLEmu::mach)

add_library(ilemu_kernel_mach_vm STATIC
    mach/vm/allocate.cpp
    mach/vm/copy.cpp
    mach/vm/deallocate.cpp
    mach/vm/inherit.cpp
    mach/vm/kernel_rpc.cpp
    mach/vm/map.cpp
    mach/vm/memory_entry.cpp
    mach/vm/protect.cpp
    mach/vm/purgable.cpp
    mach/vm/read.cpp
    mach/vm/remap.cpp
    mach/vm/region.cpp)
add_library(iLEmu::kernel_mach_vm ALIAS
    ilemu_kernel_mach_vm)
ilemu_configure_module(ilemu_kernel_mach_vm)
target_link_libraries(ilemu_kernel_mach_vm PRIVATE
    iLEmu::foundation)

add_library(ilemu_kernel_mach_input STATIC
    mach/input/graphics_services.cpp
    mach/input/graphics_services_input_abi.cpp)
add_library(iLEmu::kernel_mach_input ALIAS ilemu_kernel_mach_input)
ilemu_configure_module(ilemu_kernel_mach_input)
target_link_libraries(ilemu_kernel_mach_input PUBLIC
    iLEmu::foundation
    iLEmu::mach)

target_sources(ilemu_kernel PRIVATE
    mach/support.cpp
    mach/ipc.cpp
    mach/semaphore.cpp
    mach/message.cpp
    mach/host_message.cpp
    mach/port_message.cpp
    mach/task_vm_message.cpp
    mach/rights_message.cpp
    mach/notification_message.cpp
    mach/traps.cpp)
target_link_libraries(ilemu_kernel PRIVATE
    iLEmu::kernel_mach_transport
    iLEmu::kernel_mach_port
    iLEmu::kernel_mach_processor
    iLEmu::kernel_mach_task
    iLEmu::kernel_mach_thread
    iLEmu::kernel_mach_vm
    iLEmu::kernel_mach_input)

target_link_libraries(ilemu_kernel PRIVATE
    iLEmu::kernel_graphics
    iLEmu::device_state
    iLEmu::kernel_iokit_audio
    iLEmu::kernel_iokit_baseband
    iLEmu::kernel_iokit_camera
    iLEmu::kernel_iokit_power
    iLEmu::kernel_iokit_display
    iLEmu::kernel_iokit_graphics
    iLEmu::kernel_iokit_hid
    iLEmu::kernel_iokit_jpeg
    iLEmu::kernel_iokit_keybag
    iLEmu::kernel_iokit_mbx
    iLEmu::kernel_iokit_mobile_file_integrity)
