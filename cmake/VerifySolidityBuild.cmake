cmake_minimum_required(VERSION 3.13)

foreach(required_var IN ITEMS
	PUYA_SOL_SOLIDITY_BUILD_DIR
	PUYA_SOL_SOLIDITY_COMMIT
	PUYA_SOL_STATIC_LIBRARY_PREFIX
	PUYA_SOL_STATIC_LIBRARY_SUFFIX
)
	if(NOT DEFINED ${required_var})
		message(FATAL_ERROR "${required_var} is required")
	endif()
endforeach()

set(solidity_libraries solidity yul evmasm langutil smtutil solutil)
foreach(library IN LISTS solidity_libraries)
	set(archive "${PUYA_SOL_SOLIDITY_BUILD_DIR}/lib${library}/${PUYA_SOL_STATIC_LIBRARY_PREFIX}${library}${PUYA_SOL_STATIC_LIBRARY_SUFFIX}")
	if(NOT EXISTS "${archive}")
		message(FATAL_ERROR "Pinned Solidity build did not produce ${archive}")
	endif()
endforeach()

set(build_info "${PUYA_SOL_SOLIDITY_BUILD_DIR}/include/solidity/BuildInfo.h")
if(NOT EXISTS "${build_info}")
	message(FATAL_ERROR "Pinned Solidity build did not produce ${build_info}")
endif()

string(SUBSTRING "${PUYA_SOL_SOLIDITY_COMMIT}" 0 8 expected_short_commit)
file(READ "${build_info}" build_info_contents)
string(FIND
	"${build_info_contents}"
	"#define SOL_COMMIT_HASH \"${expected_short_commit}\""
	commit_match
)
if(commit_match EQUAL -1)
	message(FATAL_ERROR
		"Solidity BuildInfo.h does not identify pinned commit "
		"${expected_short_commit}. Refusing to link potentially stale archives.")
endif()

message(STATUS
	"Verified Solidity archives for pinned commit ${expected_short_commit}")
