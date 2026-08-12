add_library(ilemu_xnu_adapter STATIC
    xnu_mig_adapter.cpp
    ${ILEMU_MIG_GENERATED_HEADERS})
add_library(iLEmu::xnu_adapter ALIAS ilemu_xnu_adapter)
add_dependencies(ilemu_xnu_adapter ilemu_mig_metadata)
target_include_directories(ilemu_xnu_adapter PUBLIC
    ${ILEMU_PROJECT_INCLUDE_DIRS}
    $<BUILD_INTERFACE:${PROJECT_BINARY_DIR}/generated>)
target_compile_features(ilemu_xnu_adapter PUBLIC cxx_std_20)
target_compile_options(ilemu_xnu_adapter PRIVATE
    $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall;-Wextra;-Wpedantic;-Wconversion;-Wshadow>)
