if(NOT DEFINED PUYA_SOL OR NOT DEFINED SOURCE_FILE OR NOT DEFINED OUTPUT_ROOT)
    message(FATAL_ERROR "PUYA_SOL, SOURCE_FILE, and OUTPUT_ROOT are required")
endif()

get_filename_component(output_leaf "${OUTPUT_ROOT}" NAME)
if(NOT output_leaf STREQUAL "evm-layout-options-test-output")
    message(FATAL_ERROR
        "refusing to clean unexpected EVM-layout output: ${OUTPUT_ROOT}")
endif()

file(REMOVE_RECURSE "${OUTPUT_ROOT}")
file(MAKE_DIRECTORY "${OUTPUT_ROOT}")

function(run_rejected_option case_name option expected_text)
    set(case_output "${OUTPUT_ROOT}/${case_name}")
    execute_process(
        COMMAND "${PUYA_SOL}"
            "${option}"
            --log-level error
            --source "${SOURCE_FILE}"
            --output-dir "${case_output}"
            --no-puya
        RESULT_VARIABLE result
        OUTPUT_VARIABLE stdout
        ERROR_VARIABLE stderr
    )
    set(output "${stdout}\n${stderr}")
    if(NOT "${result}" STREQUAL "2")
        message(FATAL_ERROR
            "${option} returned '${result}', expected '2':\n${output}")
    endif()
    string(FIND "${output}" "${expected_text}" match_index)
    if(match_index EQUAL -1)
        message(FATAL_ERROR
            "${option} did not explain its rejection:\n${output}")
    endif()
    if(EXISTS "${case_output}/awst.json")
        message(FATAL_ERROR "${option} emitted AWST despite rejection")
    endif()
endfunction()

run_rejected_option(
    memory_layout --evm-memory-layout
    "is not implemented and cannot be enabled")
run_rejected_option(
    umbrella_layout --evm-layout
    "is unavailable because its EVM memory mode is not implemented")

# The implemented storage-only mode remains available as the explicit subset.
set(storage_output "${OUTPUT_ROOT}/storage_layout")
execute_process(
    COMMAND "${PUYA_SOL}"
        --evm-storage-layout
        --source "${SOURCE_FILE}"
        --output-dir "${storage_output}"
        --no-puya
    RESULT_VARIABLE storage_result
    OUTPUT_VARIABLE storage_stdout
    ERROR_VARIABLE storage_stderr
)
if(NOT storage_result EQUAL 0 OR NOT EXISTS "${storage_output}/awst.json")
    message(FATAL_ERROR
        "--evm-storage-layout unexpectedly failed (${storage_result}):\n"
        "${storage_stdout}\n${storage_stderr}")
endif()

execute_process(
    COMMAND "${PUYA_SOL}" --help
    RESULT_VARIABLE help_result
    OUTPUT_VARIABLE help_output
    ERROR_VARIABLE help_stderr
)
if(NOT help_result EQUAL 0)
    message(FATAL_ERROR "--help failed (${help_result}):\n${help_stderr}")
endif()
foreach(expected IN ITEMS
    "--evm-memory-layout    UNAVAILABLE"
    "--evm-layout           UNAVAILABLE"
    "--evm-storage-layout   Back all storage"
)
    string(FIND "${help_output}" "${expected}" match_index)
    if(match_index EQUAL -1)
        message(FATAL_ERROR "--help is missing '${expected}'")
    endif()
endforeach()
foreach(forbidden IN ITEMS
    "FULL EVM data-location semantics"
    "recommended mode for asm-heavy real-world contracts"
)
    string(FIND "${help_output}" "${forbidden}" match_index)
    if(NOT match_index EQUAL -1)
        message(FATAL_ERROR "--help still advertises '${forbidden}'")
    endif()
endforeach()
