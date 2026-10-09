add_library(ilemu_arm STATIC a32_decode.cpp t32_decode.cpp t32_immediate.cpp
    t32_shifted.cpp t32_transfer.cpp simd_duplicate.cpp simd_bitwise.cpp
    integer_semantics.cpp)
add_library(iLEmu::arm ALIAS ilemu_arm)
target_compile_features(ilemu_arm PUBLIC cxx_std_20)
target_include_directories(ilemu_arm PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>")
target_compile_options(ilemu_arm PRIVATE
    $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall;-Wextra;-Wconversion;-Wshadow>)
