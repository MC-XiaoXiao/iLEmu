if(NOT TARGET ilemu_softfloat)
    add_subdirectory(../../external/softfloat "${CMAKE_CURRENT_BINARY_DIR}/softfloat")
endif()
add_library(ilemu_arm STATIC floating_immediate_decode.cpp floating_comparison_decode.cpp floating_comparison.cpp floating_system_decode.cpp floating_arithmetic_decode.cpp floating_arithmetic.cpp floating_decode.cpp floating_conversion.cpp a32_decode.cpp a32_transfer.cpp t32_decode.cpp t32_immediate.cpp
    t32_shifted.cpp t32_plain_immediate.cpp t32_transfer.cpp simd_duplicate.cpp simd_immediate.cpp simd_bitwise.cpp simd_extract.cpp simd_transfer.cpp
    integer_semantics.cpp integer_transform.cpp parallel_saturating.cpp transform_decode.cpp vfp_multiple.cpp vfp_scalar.cpp core_vector_move.cpp
    multiply.cpp multiply_decode.cpp system_control.cpp program_status.cpp exclusive.cpp swap_decode.cpp dual_transfer.cpp)
add_library(iLEmu::arm ALIAS ilemu_arm)
target_link_libraries(ilemu_arm PUBLIC iLEmu::execution PRIVATE ilemu_softfloat)
target_include_directories(ilemu_arm SYSTEM PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../../external/softfloat/source/include")
target_compile_features(ilemu_arm PUBLIC cxx_std_20)
target_include_directories(ilemu_arm PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>")
target_compile_options(ilemu_arm PRIVATE
    $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall;-Wextra;-Wconversion;-Wshadow>)
