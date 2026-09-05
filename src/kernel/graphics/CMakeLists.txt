add_library(ilemu_kernel_graphics STATIC
    core_surface_hle.cpp
    layerkit_hle.cpp
    mbx2d_hle.cpp
    mbx3d_quad_hle.cpp
    mbx_client_surface.cpp
    mbx_swapchain.cpp
    mobile_framebuffer_hle.cpp
    opengles_hle.cpp
    opengles_programmable.cpp
    mbx_connect_hle.cpp)
add_library(iLEmu::kernel_graphics ALIAS ilemu_kernel_graphics)
ilemu_configure_module(ilemu_kernel_graphics)
