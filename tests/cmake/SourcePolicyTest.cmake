if(NOT DEFINED PUYA_SOL OR NOT DEFINED SOURCE_DIR OR NOT DEFINED OUTPUT_ROOT)
    message(FATAL_ERROR "PUYA_SOL, SOURCE_DIR, and OUTPUT_ROOT are required")
endif()

get_filename_component(output_leaf "${OUTPUT_ROOT}" NAME)
if(NOT output_leaf STREQUAL "source-policy-test-output")
    message(FATAL_ERROR
        "refusing to clean unexpected source-policy output: ${OUTPUT_ROOT}")
endif()

file(REMOVE_RECURSE "${OUTPUT_ROOT}")
file(MAKE_DIRECTORY "${OUTPUT_ROOT}")

function(run_source_case case_name source_name expected_result expected_text)
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
            "${case_name} emitted AWST after source-policy rejection")
    endif()
endfunction()

# Compatible source is passed through unchanged and needs no rewrite record.
run_source_case(compatible_default Minimal.sol 0 "")
if(EXISTS "${OUTPUT_ROOT}/compatible_default/source-rewrite-manifest.json")
    message(FATAL_ERROR "default compilation emitted a rewrite manifest")
endif()

# The original pragma is enforced for both entry and imported source units.
run_source_case(
    incompatible_default IncompatiblePragma.sol 1
    "Source file requires different compiler version")
run_source_case(
    incompatible_import_default LegacyImportMain.sol 1
    "Source file requires different compiler version")
run_source_case(
    incompatible_additional_default Minimal.sol 1
    "Source file requires different compiler version"
    --source "${SOURCE_DIR}/IncompatiblePragma.sol")
foreach(case_name IN ITEMS
        incompatible_default
        incompatible_import_default
        incompatible_additional_default)
    if(EXISTS "${OUTPUT_ROOT}/${case_name}/source-rewrite-manifest.json")
        message(FATAL_ERROR
            "${case_name} emitted a rewrite manifest without explicit opt-in")
    endif()
endforeach()

# Research mode is explicit, cannot be hidden by log filtering, and records
# exact original/transformed text plus both content hashes.
run_source_case(
    incompatible_opt_in IncompatiblePragma.sol 0
    "WARNING: UNSAFE LEGACY SOURCE REWRITE ENABLED"
    --legacy-source-rewrite
    --log-level error)
set(manifest
    "${OUTPUT_ROOT}/incompatible_opt_in/source-rewrite-manifest.json")
if(NOT EXISTS "${manifest}")
    message(FATAL_ERROR "legacy opt-in did not emit ${manifest}")
endif()
file(READ "${manifest}" manifest_text)
foreach(expected IN ITEMS
    "puya-sol/source-rewrite-manifest/v1"
    "IncompatiblePragma.sol"
    "original_keccak256"
    "transformed_keccak256"
    "original_source"
    "transformed_source"
    "pragma solidity <0.8.0"
    "pragma solidity >=0.8.0"
)
    string(FIND "${manifest_text}" "${expected}" match_index)
    if(match_index EQUAL -1)
        message(FATAL_ERROR "rewrite manifest is missing '${expected}'")
    endif()
endforeach()
foreach(kind IN ITEMS original transformed)
    string(REGEX MATCH
        "\"${kind}_keccak256\": \"([0-9a-f]+)\""
        hash_match "${manifest_text}")
    set("${kind}_hash" "${CMAKE_MATCH_1}")
    string(LENGTH "${CMAKE_MATCH_1}" hash_length)
    if(NOT hash_length EQUAL 64)
        message(FATAL_ERROR
            "rewrite manifest has an invalid ${kind} Keccak-256 hash")
    endif()
endforeach()
if(original_hash STREQUAL transformed_hash)
    message(FATAL_ERROR
        "changed source has identical original/transformed hashes")
endif()

run_source_case(
    incompatible_import_opt_in LegacyImportMain.sol 0
    "WARNING: UNSAFE LEGACY SOURCE REWRITE ENABLED"
    --legacy-source-rewrite)
set(import_manifest
    "${OUTPUT_ROOT}/incompatible_import_opt_in/source-rewrite-manifest.json")
file(READ "${import_manifest}" import_manifest_text)
foreach(expected IN ITEMS "LegacyImportMain.sol" "LegacyImported.sol")
    string(FIND "${import_manifest_text}" "${expected}" match_index)
    if(match_index EQUAL -1)
        message(FATAL_ERROR
            "import rewrite manifest is missing '${expected}'")
    endif()
endforeach()

run_source_case(
    incompatible_additional_opt_in Minimal.sol 0
    "WARNING: UNSAFE LEGACY SOURCE REWRITE ENABLED"
    --legacy-source-rewrite
    --source "${SOURCE_DIR}/IncompatiblePragma.sol")
set(additional_manifest
    "${OUTPUT_ROOT}/incompatible_additional_opt_in/source-rewrite-manifest.json")
file(READ "${additional_manifest}" additional_manifest_text)
foreach(expected IN ITEMS "Minimal.sol" "IncompatiblePragma.sol")
    string(FIND "${additional_manifest_text}" "${expected}" match_index)
    if(match_index EQUAL -1)
        message(FATAL_ERROR
            "additional-source rewrite manifest is missing '${expected}'")
    endif()
endforeach()
