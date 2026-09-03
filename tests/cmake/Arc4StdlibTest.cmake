if(NOT DEFINED PUYA_SOL OR NOT DEFINED SOURCE_DIR OR NOT DEFINED OUTPUT_ROOT)
    message(FATAL_ERROR "PUYA_SOL, SOURCE_DIR, and OUTPUT_ROOT are required")
endif()

get_filename_component(output_leaf "${OUTPUT_ROOT}" NAME)
if(NOT output_leaf STREQUAL "arc4-stdlib-test-output")
    message(FATAL_ERROR
        "refusing to clean unexpected ARC4 test output path: ${OUTPUT_ROOT}")
endif()

file(REMOVE_RECURSE "${OUTPUT_ROOT}")
file(MAKE_DIRECTORY "${OUTPUT_ROOT}")

function(run_frontend case_name source_name expected_result expected_text)
    set(case_output "${OUTPUT_ROOT}/${case_name}")
    execute_process(
        COMMAND "${PUYA_SOL}"
            --source "${SOURCE_DIR}/${source_name}"
            --output-dir "${case_output}"
            --no-puya
        RESULT_VARIABLE result
        OUTPUT_VARIABLE stdout
        ERROR_VARIABLE stderr
    )
    set(output "${stdout}\n${stderr}")

    if(expected_result STREQUAL "success")
        if(NOT result EQUAL 0)
            message(FATAL_ERROR
                "${case_name} unexpectedly failed (${result}):\n${output}")
        endif()
    else()
        if(result EQUAL 0)
            message(FATAL_ERROR "${case_name} unexpectedly succeeded")
        endif()
    endif()

    if(NOT expected_text STREQUAL "")
        string(FIND "${output}" "${expected_text}" match_index)
        if(match_index EQUAL -1)
            message(FATAL_ERROR
                "${case_name} output did not contain '${expected_text}':\n${output}")
        endif()
    endif()
endfunction()

run_frontend(valid Arc4StdlibValid.sol success "")
run_frontend(module_alias Arc4StdlibModuleAlias.sol success "")
run_frontend(bits_using_for BitsUsingFor.sol success "")
file(READ "${OUTPUT_ROOT}/module_alias/awst.json" module_alias_awst)
string(FIND "${module_alias_awst}" "\"op_code\": \"bitlen\"" match_index)
if(match_index EQUAL -1)
    message(FATAL_ERROR "module-aliased Bits.bitlen did not lower to AVM bitlen")
endif()
file(READ "${OUTPUT_ROOT}/bits_using_for/awst.json" bits_using_for_awst)
string(FIND "${bits_using_for_awst}" "\"op_code\": \"bitlen\"" match_index)
if(match_index EQUAL -1)
    message(FATAL_ERROR "using-for Bits.bitlen did not lower to AVM bitlen")
endif()
string(FIND "${bits_using_for_awst}" "\"name\": \"Bits.bitlen\"" match_index)
if(NOT match_index EQUAL -1)
    message(FATAL_ERROR "using-for emitted the Bits.bitlen facade body")
endif()
file(READ "${OUTPUT_ROOT}/valid/awst.json" valid_awst)
foreach(expected_type IN ITEMS "arc4.uint16" "arc4.int32")
    string(FIND "${valid_awst}" "\"name\": \"${expected_type}\"" match_index)
    if(match_index EQUAL -1)
        message(FATAL_ERROR
            "valid ARC4 envelope did not preserve ${expected_type} in AWST")
    endif()
endforeach()
string(FIND "${valid_awst}" "\"op_code\": \"bitlen\"" match_index)
if(match_index EQUAL -1)
    message(FATAL_ERROR "Bits.bitlen did not lower to the AVM bitlen intrinsic")
endif()
foreach(unexpected_subroutine IN ITEMS "ARC4.encode" "ARC4.decode" "Bits.bitlen")
    string(FIND "${valid_awst}" "\"name\": \"${unexpected_subroutine}\"" match_index)
    if(NOT match_index EQUAL -1)
        message(FATAL_ERROR
            "stdlib facade body was emitted as subroutine ${unexpected_subroutine}")
    endif()
endforeach()

run_frontend(
    invalid_encode Arc4StdlibInvalidEncode.sol failure
    "ARC4.encode must be written as ARC4.encode(abi.encode(...))")
run_frontend(
    invalid_decode Arc4StdlibInvalidDecode.sol failure
    "ARC4.decode must be used as the input to abi.decode")
run_frontend(
    function_pointer Arc4FunctionPointer.sol failure
    "ARC4.encode cannot be used as a function value")
run_frontend(
    bits_function_pointer BitsFunctionPointer.sol failure
    "Bits.bitlen cannot be used as a function value")
run_frontend(
    old_syntax Arc4OldSyntax.sol failure
    "Undeclared identifier")
run_frontend(user_named_arc4 UserNamedArc4.sol success "")
file(READ "${OUTPUT_ROOT}/user_named_arc4/awst.json" user_named_awst)
string(FIND "${user_named_awst}" "\"name\": \"ARC4.encode\"" match_index)
if(match_index EQUAL -1)
    message(FATAL_ERROR
        "user-defined ARC4 library was incorrectly consumed as the stdlib facade")
endif()

run_frontend(user_named_bits UserNamedBits.sol success "")
file(READ "${OUTPUT_ROOT}/user_named_bits/awst.json" user_named_bits_awst)
string(FIND "${user_named_bits_awst}" "\"name\": \"Bits.bitlen\"" match_index)
if(match_index EQUAL -1)
    message(FATAL_ERROR
        "user-defined Bits library was incorrectly consumed as a stdlib intrinsic")
endif()
