add_executable(ilemu_dyld_shared_cache_tests dyld_shared_cache_tests.cpp)
target_link_libraries(ilemu_dyld_shared_cache_tests PRIVATE
    iLEmu::core
    ilemu_test_support)
add_test(NAME dyld_shared_cache COMMAND ilemu_dyld_shared_cache_tests)
set_tests_properties(dyld_shared_cache PROPERTIES
    WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}")
