cmake_minimum_required(VERSION 3.13)

function(puya_sol_verify_solidity_checkout root_dir solidity_dir output_commit)
	find_package(Git QUIET)
	if(NOT Git_FOUND AND NOT GIT_FOUND)
		message(FATAL_ERROR
			"Git is required to verify the pinned Solidity submodule checkout")
	endif()

	if(NOT EXISTS "${solidity_dir}/CMakeLists.txt")
		message(FATAL_ERROR
			"The Solidity submodule is not initialized at ${solidity_dir}.\n"
			"Run: git submodule update --init --recursive")
	endif()

	execute_process(
		COMMAND "${GIT_EXECUTABLE}" -C "${root_dir}"
			submodule status --recursive -- solidity
		RESULT_VARIABLE submodule_result
		OUTPUT_VARIABLE submodule_status
		ERROR_VARIABLE submodule_error
		OUTPUT_STRIP_TRAILING_WHITESPACE
	)
	if(NOT submodule_result EQUAL 0)
		message(FATAL_ERROR
			"Unable to inspect the Solidity submodule:\n${submodule_error}")
	endif()

	string(REPLACE "\n" ";" submodule_lines "${submodule_status}")
	set(mismatched_submodules "")
	foreach(line IN LISTS submodule_lines)
		if(line STREQUAL "")
			continue()
		endif()
		string(SUBSTRING "${line}" 0 1 marker)
		if(NOT marker STREQUAL " ")
			string(APPEND mismatched_submodules "\n  ${line}")
		endif()
	endforeach()
	if(NOT mismatched_submodules STREQUAL "")
		message(FATAL_ERROR
			"Solidity or one of its nested submodules is not at the commit pinned "
			"by this checkout:${mismatched_submodules}\n"
			"Run: git submodule update --init --recursive")
	endif()

	execute_process(
		COMMAND "${GIT_EXECUTABLE}" -C "${solidity_dir}"
			status --porcelain=v1 --untracked-files=all --ignore-submodules=none
		RESULT_VARIABLE dirty_result
		OUTPUT_VARIABLE dirty_status
		ERROR_VARIABLE dirty_error
		OUTPUT_STRIP_TRAILING_WHITESPACE
	)
	if(NOT dirty_result EQUAL 0)
		message(FATAL_ERROR
			"Unable to inspect the Solidity working tree:\n${dirty_error}")
	endif()
	if(NOT dirty_status STREQUAL "")
		message(FATAL_ERROR
			"The pinned Solidity working tree must be clean. Found:\n"
			"${dirty_status}\n"
			"Commit or remove those changes before building puya-sol.")
	endif()

	execute_process(
		COMMAND "${GIT_EXECUTABLE}" -C "${solidity_dir}" rev-parse --verify HEAD
		RESULT_VARIABLE revision_result
		OUTPUT_VARIABLE revision
		ERROR_VARIABLE revision_error
		OUTPUT_STRIP_TRAILING_WHITESPACE
	)
	string(LENGTH "${revision}" revision_length)
	if(NOT revision_result EQUAL 0
		OR NOT revision_length EQUAL 40
		OR NOT revision MATCHES "^[0-9a-fA-F]+$")
		message(FATAL_ERROR
			"Unable to determine the pinned Solidity revision:\n${revision_error}")
	endif()

	set(${output_commit} "${revision}" PARENT_SCOPE)
endfunction()

if(CMAKE_SCRIPT_MODE_FILE)
	if(NOT DEFINED PUYA_SOL_ROOT_DIR OR NOT DEFINED PUYA_SOL_SOLIDITY_DIR)
		message(FATAL_ERROR
			"PUYA_SOL_ROOT_DIR and PUYA_SOL_SOLIDITY_DIR are required")
	endif()
	puya_sol_verify_solidity_checkout(
		"${PUYA_SOL_ROOT_DIR}"
		"${PUYA_SOL_SOLIDITY_DIR}"
		verified_revision
	)
	message(STATUS "Verified pinned Solidity checkout ${verified_revision}")
endif()
