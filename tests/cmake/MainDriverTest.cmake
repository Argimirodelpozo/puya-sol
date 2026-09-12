if(NOT DEFINED PUYA_SOL OR NOT DEFINED OUTPUT_ROOT)
    message(FATAL_ERROR "PUYA_SOL and OUTPUT_ROOT are required")
endif()
get_filename_component(output_leaf "${OUTPUT_ROOT}" NAME)
if(NOT output_leaf STREQUAL "main-driver-test-output")
    message(FATAL_ERROR "refusing to clean unexpected test output: ${OUTPUT_ROOT}")
endif()
file(REMOVE_RECURSE "${OUTPUT_ROOT}")
file(MAKE_DIRECTORY "${OUTPUT_ROOT}/input/left" "${OUTPUT_ROOT}/input/right")
set(input "${OUTPUT_ROOT}/input")
file(WRITE "${input}/Minimal.sol" "// SPDX-License-Identifier: MIT\npragma solidity ^0.8.20;\ncontract C { function value() external pure returns(uint256) { return 17; } }\n")

function(run_case name expected text)
    execute_process(COMMAND "${PUYA_SOL}" --no-puya ${ARGN}
        RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE errors)
    if(NOT "${result}" STREQUAL "${expected}")
        message(FATAL_ERROR "${name}: exit '${result}', expected '${expected}':\n${output}\n${errors}")
    endif()
    string(FIND "${output}\n${errors}" "${text}" found)
    if(NOT text STREQUAL "" AND found EQUAL -1)
        message(FATAL_ERROR "${name}: missing '${text}':\n${output}\n${errors}")
    endif()
    set(case_stdout "${output}" PARENT_SCOPE)
    set(case_stderr "${errors}" PARENT_SCOPE)
endfunction()

# Distinct paths can collide after solc strips base/include prefixes. Neither
# source order is allowed to choose which body wins. Real path aliases remain
# legal and are already covered by SourceIdentityTest.cmake.
foreach(side left right)
    file(WRITE "${input}/${side}/Same.sol" "pragma solidity ^0.8.20; contract C { function ${side}() external pure returns(uint256) { return 1; } }")
endforeach()
foreach(first left right)
    if(first STREQUAL "left")
        set(second right)
    else()
        set(second left)
    endif()
    set(destination "${OUTPUT_ROOT}/collision-${first}")
    run_case("collision-${first}" 1 "Source unit name collision detected"
        --source "${input}/${first}/Same.sol" --source "${input}/${second}/Same.sol"
        --import-path "${input}/${first}" --import-path "${input}/${second}"
        --output-dir "${destination}")
    if(EXISTS "${destination}/awst.json" OR EXISTS "${destination}/artifact-manifest.json")
        message(FATAL_ERROR "colliding source units published artifacts")
    endif()
endforeach()

# Output/log failures must be ordinary diagnostics, never SIGABRT. Disabling
# log output must bypass only the log file, not the output-directory checks.
file(WRITE "${OUTPUT_ROOT}/not-a-directory" "preserve")
foreach(logs IN ITEMS enabled disabled)
    set(flags)
    if(logs STREQUAL "disabled")
        set(flags --no-output-logs)
    endif()
    run_case("file-output-${logs}" 1 "Cannot create artifact output directory"
        --source "${input}/Minimal.sol" --output-dir "${OUTPUT_ROOT}/not-a-directory" ${flags})
endforeach()
file(READ "${OUTPUT_ROOT}/not-a-directory" untouched)
if(NOT untouched STREQUAL "preserve")
    message(FATAL_ERROR "invalid output directory overwrote an existing file")
endif()
file(MAKE_DIRECTORY "${OUTPUT_ROOT}/log-blocked/puya-sol.log")
run_case(log-blocked 1 "Cannot open compilation log" --source "${input}/Minimal.sol"
    --output-dir "${OUTPUT_ROOT}/log-blocked")
run_case(log-disabled 0 "" --source "${input}/Minimal.sol"
    --output-dir "${OUTPUT_ROOT}/log-blocked" --no-output-logs)

# Option-only validation precedes filesystem writes and source reads. An
# invalid new configuration must not even truncate a prior compilation log.
file(MAKE_DIRECTORY "${OUTPUT_ROOT}/early")
file(WRITE "${OUTPUT_ROOT}/early/puya-sol.log" "previous log")
file(WRITE "${OUTPUT_ROOT}/early/artifact-manifest.json" "previous marker")
set(early --source "${input}/absent.sol" --output-dir "${OUTPUT_ROOT}/early")
run_case(version-early 2 "Unknown EVM version" ${early} --evm-version definitely-not-a-fork)
run_case(xchain-abi-early 2 "requires --contract-abi evm" ${early}
    --xchain-template eeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeee)
run_case(xchain-placeholder-early 2 "owner placeholder exactly once" ${early}
    --contract-abi evm --xchain-template 0011)
file(READ "${OUTPUT_ROOT}/early/puya-sol.log" old_log)
file(READ "${OUTPUT_ROOT}/early/artifact-manifest.json" old_marker)
if(NOT old_log STREQUAL "previous log" OR NOT old_marker STREQUAL "previous marker")
    message(FATAL_ERROR "invalid options modified prior artifacts")
endif()

# Successful analysis still reports solc warnings with their IDs/locations.
file(WRITE "${input}/Warnings.sol" "// SPDX-License-Identifier: MIT\npragma solidity ^0.8.20;\ncontract W {\nfunction f(uint256 unused) external pure returns(uint256) { return 1; }\n}\n")
run_case(warnings 0 "Unused function parameter" --source "${input}/Warnings.sol"
    --output-dir "${OUTPUT_ROOT}/warnings" --dump-awst)
if(NOT case_stderr MATCHES "Warning[^\n]*5667" OR NOT case_stderr MATCHES "Warnings.sol:4:")
    message(FATAL_ERROR "solc warning lost its ID or source range: ${case_stderr}")
endif()
file(READ "${OUTPUT_ROOT}/warnings/awst.json" awst)
if(NOT awst STREQUAL case_stdout)
    message(FATAL_ERROR "--dump-awst must match the published bytes exactly")
endif()
run_case(quiet-warnings 0 "" --source "${input}/Warnings.sol"
    --output-dir "${OUTPUT_ROOT}/quiet-warnings" --log-level error)
if(case_stderr MATCHES "Warning|Unused function parameter")
    message(FATAL_ERROR "solc warnings ignored the requested log level")
endif()

# Solc's formatter supplies secondary locations without a local reimplementation.
file(WRITE "${input}/Duplicate.sol" "// SPDX-License-Identifier: MIT\npragma solidity ^0.8.20;\ncontract D { function f() public {} function f() public {} }\n")
run_case(secondary-location 1 "Other declaration is here" --source "${input}/Duplicate.sol"
    --output-dir "${OUTPUT_ROOT}/secondary-location")
if(NOT case_stderr MATCHES "Duplicate.sol:3:" OR EXISTS "${OUTPUT_ROOT}/secondary-location/awst.json")
    message(FATAL_ERROR "failed analysis lost its range or emitted AWST: ${case_stderr}")
endif()

# The same normalized source is not transformed twice when explicitly repeated.
file(WRITE "${input}/Legacy.sol" "pragma solidity <0.8.0; contract Legacy { function f() public pure returns(uint) { return 1; } }")
run_case(legacy-once 0 "UNSAFE LEGACY SOURCE REWRITE" --source "${input}/Legacy.sol"
    --source "${input}/./Legacy.sol" --legacy-source-rewrite --output-dir "${OUTPUT_ROOT}/legacy-once")
file(READ "${OUTPUT_ROOT}/legacy-once/source-rewrite-manifest.json" rewrites)
string(FIND "${rewrites}" "pragma solidity <0.8.0" original)
if(original EQUAL -1)
    message(FATAL_ERROR "repeated legacy input lost the original source")
endif()
