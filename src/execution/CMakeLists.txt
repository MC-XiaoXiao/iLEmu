# This target can also configure without the simulator or any JIT dependency.
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    cmake_minimum_required(VERSION 3.20)
    project(ilemu_execution LANGUAGES CXX)
endif()

add_library(ilemu_execution INTERFACE)
add_library(iLEmu::execution ALIAS ilemu_execution)
target_compile_features(ilemu_execution INTERFACE cxx_std_20)
target_include_directories(ilemu_execution INTERFACE
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>")

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    add_subdirectory(../arm arm)
    add_subdirectory(../arm_memory arm_memory)
    add_subdirectory(../arm_interpreter arm_interpreter)
    add_subdirectory(../arm64 arm64)
    add_subdirectory(../host_execution host_execution)
endif()
