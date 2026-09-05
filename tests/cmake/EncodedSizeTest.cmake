foreach(case IN ITEMS OversizedEncodedArray OversizedArrayLength)
    execute_process(
        COMMAND "${PUYA_SOL}" --source "${SOURCE_DIR}/${case}.sol"
            --output-dir "${OUTPUT_ROOT}/${case}" --no-puya --log-level error
        RESULT_VARIABLE result
        OUTPUT_VARIABLE stdout
        ERROR_VARIABLE stderr
        TIMEOUT 15
    )
    if(NOT "${result}" STREQUAL "1")
        message(FATAL_ERROR "${case}: expected diagnostic exit 1, got ${result}:\n${stdout}\n${stderr}")
    endif()
    if(NOT "${stdout}\n${stderr}" MATCHES "(encoded element size|Solidity array length) exceeds the compiler's addressable range")
        message(FATAL_ERROR "${case}: missing checked-capacity diagnostic:\n${stdout}\n${stderr}")
    endif()
    if(EXISTS "${OUTPUT_ROOT}/${case}/artifact-manifest.json")
        message(FATAL_ERROR "${case}: unsupported size produced a successful artifact manifest")
    endif()
endforeach()
