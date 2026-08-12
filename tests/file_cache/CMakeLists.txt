add_executable(ilemu_file_page_cache_tests file_page_cache_tests.cpp)
target_link_libraries(ilemu_file_page_cache_tests PRIVATE
    iLEmu::core
    ilemu_test_support)
add_test(NAME file_page_cache COMMAND ilemu_file_page_cache_tests)
set_tests_properties(file_page_cache PROPERTIES
    WORKING_DIRECTORY "${ILEMU_WORKSPACE_DIR}")
