add_library(ilemu_app_control STATIC
    live_control.cpp
    live_button_scheduler.cpp
    live_touch_scheduler.cpp)
add_library(iLEmu::app_control ALIAS ilemu_app_control)
ilemu_configure_module(ilemu_app_control)
target_link_libraries(ilemu_app_control PUBLIC
    iLEmu::foundation)
