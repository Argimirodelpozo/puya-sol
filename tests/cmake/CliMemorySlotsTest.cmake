if(NOT DEFINED PUYA_SOL)
    message(FATAL_ERROR "PUYA_SOL is required")
endif()

function(run_memory_slots_case case_name value expected_result)
    execute_process(
        COMMAND "${PUYA_SOL}" --evm-memory-slots "${value}" --help
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

    if(expected_result EQUAL 0)
        string(FIND "${output}" "max 88" max_index)
        if(max_index EQUAL -1)
            message(FATAL_ERROR
                "${case_name} help does not advertise max 88:\n${output}")
        endif()
    else()
        string(FIND "${output}" "expects a value in [1, 88]" error_index)
        if(error_index EQUAL -1)
            message(FATAL_ERROR
                "${case_name} did not report the valid range:\n${output}")
        endif()
    endif()
endfunction()

run_memory_slots_case(minimum 1 0)
run_memory_slots_case(maximum 88 0)
run_memory_slots_case(zero 0 2)
run_memory_slots_case(above_maximum 89 2)
