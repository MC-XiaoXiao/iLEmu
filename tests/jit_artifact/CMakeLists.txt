add_executable(ilemu_jit_artifact_tests jit_artifact_tests.cpp)
target_link_libraries(ilemu_jit_artifact_tests PRIVATE
    iLEmu::core
    ilemu_test_support)
add_test(NAME jit_artifact COMMAND ilemu_jit_artifact_tests)
set_tests_properties(jit_artifact PROPERTIES
    WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}")
