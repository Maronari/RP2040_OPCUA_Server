find_package(Git REQUIRED)

function(rp2_apply_submodule_patch submodule_relative_path patch_relative_path)
    set(submodule_path "${CMAKE_SOURCE_DIR}/${submodule_relative_path}")
    set(patch_path "${CMAKE_SOURCE_DIR}/${patch_relative_path}")

    if(NOT EXISTS "${submodule_path}/.git")
        message(FATAL_ERROR
            "Submodule ${submodule_relative_path} is not initialized. "
            "Run: git submodule update --init --recursive")
    endif()
    if(NOT EXISTS "${patch_path}")
        message(FATAL_ERROR "Missing submodule patch: ${patch_path}")
    endif()

    execute_process(
        COMMAND "${GIT_EXECUTABLE}" apply --check --whitespace=nowarn "${patch_path}"
        WORKING_DIRECTORY "${submodule_path}"
        RESULT_VARIABLE patch_can_apply
        OUTPUT_QUIET
        ERROR_QUIET)

    if(patch_can_apply EQUAL 0)
        execute_process(
            COMMAND "${GIT_EXECUTABLE}" apply --whitespace=nowarn "${patch_path}"
            WORKING_DIRECTORY "${submodule_path}"
            RESULT_VARIABLE patch_result
            OUTPUT_VARIABLE patch_output
            ERROR_VARIABLE patch_error)
        if(NOT patch_result EQUAL 0)
            message(FATAL_ERROR
                "Failed to apply ${patch_relative_path}: ${patch_output}${patch_error}")
        endif()
        message(STATUS "Applied submodule patch: ${patch_relative_path}")
        return()
    endif()

    execute_process(
        COMMAND "${GIT_EXECUTABLE}" apply --reverse --check --whitespace=nowarn "${patch_path}"
        WORKING_DIRECTORY "${submodule_path}"
        RESULT_VARIABLE patch_is_applied
        OUTPUT_QUIET
        ERROR_QUIET)
    if(NOT patch_is_applied EQUAL 0)
        message(FATAL_ERROR
            "Patch ${patch_relative_path} is incompatible with ${submodule_relative_path}")
    endif()

    message(STATUS "Submodule patch already applied: ${patch_relative_path}")
endfunction()
