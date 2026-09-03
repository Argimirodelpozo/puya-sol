if(NOT DEFINED PUYA_SOL)
    message(FATAL_ERROR "PUYA_SOL is required")
endif()

set(removed_options
    --split-contracts
    --allow-mid-function-split
    --split-config
    --force-delegate
    --fn-split
    --pin-to-main
    --deploy-pure-helpers
    --pure-helper-split
)

execute_process(
    COMMAND "${PUYA_SOL}" --help
    RESULT_VARIABLE help_result
    OUTPUT_VARIABLE help_output
    ERROR_VARIABLE help_stderr
)
if(NOT help_result EQUAL 0)
    message(FATAL_ERROR "--help failed (${help_result}):\n${help_stderr}")
endif()

foreach(option IN LISTS removed_options)
    string(FIND "${help_output}" "${option}" help_index)
    if(NOT help_index EQUAL -1)
        message(FATAL_ERROR "removed option ${option} is still advertised")
    endif()

    execute_process(
        COMMAND "${PUYA_SOL}" "${option}"
        RESULT_VARIABLE result
        OUTPUT_VARIABLE stdout
        ERROR_VARIABLE stderr
    )
    set(output "${stdout}\n${stderr}")
    if(NOT "${result}" STREQUAL "1")
        message(FATAL_ERROR
            "removed option ${option} returned '${result}', expected '1':\n${output}")
    endif()
    string(FIND "${output}" "Unknown option: ${option}" error_index)
    if(error_index EQUAL -1)
        message(FATAL_ERROR
            "removed option ${option} did not produce an unknown-option error:\n${output}")
    endif()
endforeach()
