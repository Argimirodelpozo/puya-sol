if(NOT DEFINED PUYA_SOL OR NOT DEFINED SOURCE_DIR OR NOT DEFINED OUTPUT_ROOT)
    message(FATAL_ERROR "PUYA_SOL, SOURCE_DIR, and OUTPUT_ROOT are required")
endif()

get_filename_component(output_leaf "${OUTPUT_ROOT}" NAME)
if(NOT output_leaf STREQUAL "divergence-policy-test-output")
    message(FATAL_ERROR
        "refusing to clean unexpected divergence test output: ${OUTPUT_ROOT}")
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
    if(NOT expected_text STREQUAL "")
        string(FIND "${output}" "${expected_text}" match_index)
        if(match_index EQUAL -1)
            message(FATAL_ERROR
                "${case_name} output did not contain "
                "'${expected_text}':\n${output}")
        endif()
    endif()

    if(expected_result EQUAL 0)
        if(NOT EXISTS "${case_output}/awst.json")
            message(FATAL_ERROR "${case_name} did not emit awst.json")
        endif()
    elseif(EXISTS "${case_output}/awst.json")
        message(FATAL_ERROR
            "${case_name} emitted AWST despite a denied divergence")
    endif()
    set(frontend_output "${output}" PARENT_SCOPE)
    set(frontend_awst_path "${case_output}/awst.json" PARENT_SCOPE)
endfunction()

# Errors remain visible at --log-level error, so log filtering cannot turn an
# unapproved adaptation into an apparently successful compile.
run_frontend(
    basefee_denied BlockBaseFee.sol 1
    "--allow-divergence block-basefee"
    --log-level error)
run_frontend(
    basefee_allowed BlockBaseFee.sol 0 ""
    --allow-divergence block-basefee)

run_frontend(
    address_balance_denied AddressBalance.sol 1
    "--allow-divergence address-balance-units")
run_frontend(
    address_balance_allowed AddressBalance.sol 0 ""
    --allow-divergence address-balance-units)

run_frontend(
    native_value_denied NativeValueTransfer.sol 1
    "--allow-divergence native-value-transfer"
    --contract-abi evm)
run_frontend(
    native_value_allowed NativeValueTransfer.sol 0 ""
    --contract-abi evm
    --allow-divergence native-value-transfer)

# Every source-level payment route uses the same receiver/policy boundary,
# including assembly words and selfdestruct's CloseRemainderTo beneficiary.
set(payment_template "eeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeee")
foreach(payment_source NativeValueTransfer NativeValueSelfdestruct NativeValueAssembly)
    run_frontend(
        ${payment_source}_denied ${payment_source}.sol 1
        "--allow-divergence native-value-transfer"
        --contract-abi evm --log-level error)
    run_frontend(
        ${payment_source}_allowed ${payment_source}.sol 0
        "[allowed AVM adaptation: native value transfer]"
        --contract-abi evm --allow-divergence native-value-transfer)
    run_frontend(
        ${payment_source}_native ${payment_source}.sol 0 "")
    run_frontend(
        ${payment_source}_xchain ${payment_source}.sol 0 ""
        --contract-abi evm --xchain-template "${payment_template}")
    file(READ "${frontend_awst_path}" payment_awst)
    foreach(required_text "sha512_256" "payment target application does not exist")
        string(FIND "${payment_awst}" "${required_text}" found)
        if(found EQUAL -1)
            message(FATAL_ERROR "${payment_source} omitted receiver handling: ${required_text}")
        endif()
    endforeach()
    if(payment_source STREQUAL "NativeValueSelfdestruct")
        string(FIND "${payment_awst}" "CloseRemainderTo" found)
    else()
        string(FIND "${payment_awst}" "transfer amount exceeds uint64" found)
    endif()
    if(found EQUAL -1)
        message(FATAL_ERROR "${payment_source} omitted close/amount handling")
    endif()
endforeach()

# No payment is emitted for a constant-zero Yul call. Solc-typed application
# calls and new-app funding have a proven native escrow, not a lossy account.
run_frontend(native_zero_assembly NativeValueZeroAssembly.sol 0 "" --contract-abi evm)
run_frontend(native_known_app NativeValueKnownApp.sol 0 "" --contract-abi evm)

# Missing static-call write protection only warns. The independent low-level
# call outcome divergence still requires acknowledgment.
run_frontend(
    staticcall_outcome_denied StaticCall.sol 1
    "--allow-divergence low-level-call-outcome")
run_frontend(
    staticcall_partially_allowed StaticCall.sol 1
    "--allow-divergence low-level-call-outcome"
    --allow-divergence staticcall)
run_frontend(
    staticcall_warns StaticCall.sol 0
    "[allowed AVM adaptation: staticcall] AVM does not enforce the EVM read-only guarantee"
    --allow-divergence low-level-call-outcome)
run_frontend(
    staticcall_legacy_opt_in StaticCall.sol 0
    "[allowed AVM adaptation: staticcall]"
    --allow-divergence staticcall
    --allow-divergence low-level-call-outcome)
run_frontend(
    staticcall_warning_filtered StaticCall.sol 0 ""
    --allow-divergence low-level-call-outcome --log-level error)

run_frontend(
    typed_staticcall_warns TypedStaticCall.sol 0
    "[allowed AVM adaptation: staticcall] AVM does not enforce the EVM read-only guarantee")
string(REGEX MATCHALL "\\[allowed AVM adaptation: staticcall\\]" static_warnings "${frontend_output}")
list(LENGTH static_warnings static_warning_count)
if(NOT static_warning_count EQUAL 4)
    message(FATAL_ERROR
        "expected warnings for view/pure/getter/pointer calls only, got ${static_warning_count}:\n${frontend_output}")
endif()
run_frontend(
    assembly_staticcall_warns AssemblyStaticCall.sol 0
    "[allowed AVM adaptation: staticcall] AVM does not enforce the EVM read-only guarantee")

run_frontend(
    delegatecall_denied DelegateCall.sol 1
    "--allow-divergence delegatecall")
run_frontend(
    delegatecall_allowed DelegateCall.sol 0 ""
    --allow-divergence delegatecall)

run_frontend(
    self_call_denied SelfCall.sol 1
    "--allow-divergence self-call")
run_frontend(
    self_call_allowed SelfCall.sol 0 ""
    --allow-divergence self-call
    --allow-divergence low-level-call-outcome)

run_frontend(
    try_catch_denied TryCatch.sol 1
    "--allow-divergence try-catch")
run_frontend(
    try_catch_allowed TryCatch.sol 0 ""
    --allow-divergence try-catch
    --allow-divergence low-level-call-outcome)

run_frontend(
    unknown_opt_in Minimal.sol 2
    "--allow-divergence does not recognize 'all'"
    --allow-divergence all)
