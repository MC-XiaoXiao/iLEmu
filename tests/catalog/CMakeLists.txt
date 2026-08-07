add_executable(ilemu_executable_catalog_tests catalog_tests.cpp)
target_link_libraries(ilemu_executable_catalog_tests PRIVATE
    iLEmu::core
    ilemu_test_support)
add_test(NAME executable_catalog COMMAND ilemu_executable_catalog_tests)
set_tests_properties(executable_catalog PROPERTIES
    WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}")
