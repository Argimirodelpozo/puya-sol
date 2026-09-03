if(NOT DEFINED PUYA_SOL OR NOT DEFINED SOURCE_FILE OR NOT DEFINED OUTPUT_ROOT)
    message(FATAL_ERROR "PUYA_SOL, SOURCE_FILE, and OUTPUT_ROOT are required")
endif()

get_filename_component(output_leaf "${OUTPUT_ROOT}" NAME)
if(NOT output_leaf STREQUAL "xchain-cli-test-output")
    message(FATAL_ERROR
        "refusing to clean unexpected xchain test output: ${OUTPUT_ROOT}")
endif()

file(REMOVE_RECURSE "${OUTPUT_ROOT}")
file(MAKE_DIRECTORY "${OUTPUT_ROOT}")

function(run_xchain_case case_name expected_result expected_text template_hex)
    set(case_output "${OUTPUT_ROOT}/${case_name}")
    execute_process(
        COMMAND "${PUYA_SOL}"
            --source "${SOURCE_FILE}"
            --output-dir "${case_output}"
            --no-puya
            --contract-abi evm
            --xchain-template "${template_hex}"
            ${ARGN}
        RESULT_VARIABLE result
        OUTPUT_VARIABLE stdout
        ERROR_VARIABLE stderr
    )
    set(output "${stdout}\n${stderr}")

    if(NOT "${result}" STREQUAL "${expected_result}")
        message(FATAL_ERROR
            "${case_name} returned '${result}', expected "
            "'${expected_result}':\n${output}")
    endif()
    string(FIND "${output}" "${expected_text}" match_index)
    if(NOT expected_text STREQUAL "" AND match_index EQUAL -1)
        message(FATAL_ERROR
            "${case_name} output did not contain "
            "'${expected_text}':\n${output}")
    endif()
endfunction()

set(default_placeholder "eeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeee")
set(custom_placeholder "0123456789abcdef0123456789abcdef01234567")

# The placeholder alone is the smallest valid template. Uppercase prefixes and
# digits are accepted consistently for both template and placeholder.
run_xchain_case(minimum 0 "" "${default_placeholder}")
run_xchain_case(
    mixed_case 0 ""
    "0XAA${custom_placeholder}BB"
    --xchain-placeholder "0X${custom_placeholder}")

run_xchain_case(empty 2 "--xchain-template expects" "")
run_xchain_case(odd_length 2 "--xchain-template expects" "abc")
run_xchain_case(invalid_first_nibble 2 "--xchain-template expects" "g0")
run_xchain_case(invalid_second_nibble 2 "--xchain-template expects" "0g")
run_xchain_case(
    short_placeholder 2 "--xchain-placeholder expects 20 bytes"
    "${default_placeholder}"
    --xchain-placeholder "ee")
run_xchain_case(
    missing_placeholder 2
    "must contain the owner placeholder exactly once"
    "00112233445566778899aabbccddeeff")
run_xchain_case(
    duplicate_placeholder 2
    "must contain the owner placeholder exactly once"
    "${default_placeholder}${default_placeholder}")
