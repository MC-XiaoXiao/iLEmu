# Apply a tracked dependency patch, accepting an already patched checkout.
function(ilemu_apply_dependency_patch source_dir patch_file)
    execute_process(
        COMMAND git apply --check --unidiff-zero "${patch_file}"
        WORKING_DIRECTORY "${source_dir}"
        RESULT_VARIABLE check_result OUTPUT_QUIET ERROR_VARIABLE patch_error)
    if(check_result EQUAL 0)
        execute_process(
            COMMAND git apply --unidiff-zero "${patch_file}"
            WORKING_DIRECTORY "${source_dir}"
            RESULT_VARIABLE apply_result OUTPUT_QUIET ERROR_VARIABLE patch_error)
        if(NOT apply_result EQUAL 0)
            message(FATAL_ERROR "Failed to apply ${patch_file}: ${patch_error}")
        endif()
    else()
        execute_process(
            COMMAND git apply --check --unidiff-zero --reverse "${patch_file}"
            WORKING_DIRECTORY "${source_dir}"
            RESULT_VARIABLE reverse_result OUTPUT_QUIET ERROR_VARIABLE reverse_error)
        if(NOT reverse_result EQUAL 0)
            message(FATAL_ERROR
                "Dependency is incompatible with ${patch_file}: ${patch_error}; ${reverse_error}")
        endif()
    endif()
endfunction()
