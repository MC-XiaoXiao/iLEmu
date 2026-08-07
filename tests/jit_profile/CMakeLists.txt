add_executable(ilemu_jit_translation_profile_tests jit_translation_profile_tests.cpp)
target_link_libraries(ilemu_jit_translation_profile_tests PRIVATE
    iLEmu::core
    ilemu_test_support)
add_test(NAME jit_translation_profile COMMAND ilemu_jit_translation_profile_tests)
set_tests_properties(jit_translation_profile PROPERTIES
    WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}")
