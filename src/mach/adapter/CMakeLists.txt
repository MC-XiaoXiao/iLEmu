add_library(ilemu_xnu_adapter STATIC
    xnu_mig_adapter.cpp
    ${ILEMU_MIG_GENERATED_HEADERS})
add_library(iLEmu::xnu_adapter ALIAS ilemu_xnu_adapter)
add_dependencies(ilemu_xnu_adapter ilemu_mig_metadata)
ilemu_configure_module(ilemu_xnu_adapter)
target_include_directories(ilemu_xnu_adapter PUBLIC
    $<BUILD_INTERFACE:${PROJECT_BINARY_DIR}/generated>)
