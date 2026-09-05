add_library(ilemu_xnu_adapter STATIC xnu_mig_adapter.cpp)
add_library(iLEmu::xnu_adapter ALIAS ilemu_xnu_adapter)
ilemu_configure_module(ilemu_xnu_adapter)
