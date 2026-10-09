add_library(ilemu_arm_interpreter STATIC interpreter.cpp)
add_library(iLEmu::arm_interpreter ALIAS ilemu_arm_interpreter)
target_compile_features(ilemu_arm_interpreter PUBLIC cxx_std_20)
target_include_directories(ilemu_arm_interpreter PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>")
target_link_libraries(ilemu_arm_interpreter PUBLIC iLEmu::execution PRIVATE iLEmu::arm iLEmu::arm_memory)
target_compile_options(ilemu_arm_interpreter PRIVATE
    $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall;-Wextra;-Wconversion;-Wshadow>)
