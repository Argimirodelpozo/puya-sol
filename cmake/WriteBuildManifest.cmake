foreach(required_var IN ITEMS
    PUYA_SOL_ROOT_DIR
    PUYA_SOL_BINARY
    PUYA_SOL_OUTPUT
    PUYA_SOL_SOLIDITY_COMMIT
    PUYA_SOL_STDLIB
    PUYA_SOL_CXX_COMPILER
    PUYA_SOL_BUILD_TYPE
    PUYA_SOL_BOOST_VERSION
    PUYA_SOL_BOOST_INCLUDE_DIR
    PUYA_SOL_BOOST_FILESYSTEM_LIBRARY
)
    if(NOT DEFINED ${required_var} OR "${${required_var}}" STREQUAL "")
        message(FATAL_ERROR "${required_var} is required")
    endif()
endforeach()

find_package(Git REQUIRED)

function(run_checked output_var description)
    execute_process(
        COMMAND ${ARGN}
        RESULT_VARIABLE result
        OUTPUT_VARIABLE output
        ERROR_VARIABLE error
        OUTPUT_STRIP_TRAILING_WHITESPACE
    )
    if(NOT result EQUAL 0)
        message(FATAL_ERROR "${description} failed (${result}): ${error}")
    endif()
    set(${output_var} "${output}" PARENT_SCOPE)
endfunction()

run_checked(root_commit "reading the root commit"
    "${GIT_EXECUTABLE}" -C "${PUYA_SOL_ROOT_DIR}" rev-parse HEAD)
run_checked(submodule_status "reading recursive submodule revisions"
    "${GIT_EXECUTABLE}" -C "${PUYA_SOL_ROOT_DIR}"
    submodule status --recursive)
run_checked(tree_status "reading the checkout state"
    "${GIT_EXECUTABLE}" -C "${PUYA_SOL_ROOT_DIR}"
    status --porcelain=v1 --untracked-files=all --ignore-submodules=none)
run_checked(compiler_version "reading the C++ compiler version"
    "${PUYA_SOL_CXX_COMPILER}" --version)

if(tree_status STREQUAL "")
    set(tree_state clean)
else()
    set(tree_state dirty)
endif()

file(SHA256 "${PUYA_SOL_BINARY}" binary_sha256)
file(SHA256 "${PUYA_SOL_STDLIB}" stdlib_sha256)

file(WRITE "${PUYA_SOL_OUTPUT}"
    "root_commit=${root_commit}\n"
    "root_tree_state=${tree_state}\n"
    "solidity_commit=${PUYA_SOL_SOLIDITY_COMMIT}\n"
    "build_type=${PUYA_SOL_BUILD_TYPE}\n"
    "cmake_version=${CMAKE_VERSION}\n"
    "cxx_compiler=${PUYA_SOL_CXX_COMPILER}\n"
    "boost_version=${PUYA_SOL_BOOST_VERSION}\n"
    "boost_include_dir=${PUYA_SOL_BOOST_INCLUDE_DIR}\n"
    "boost_filesystem_library=${PUYA_SOL_BOOST_FILESYSTEM_LIBRARY}\n"
    "puya_sol_sha256=${binary_sha256}\n"
    "avm_stdlib_sha256=${stdlib_sha256}\n"
    "\n[submodules]\n${submodule_status}\n"
    "\n[cxx_compiler_version]\n${compiler_version}\n"
)

message(STATUS "Wrote build manifest: ${PUYA_SOL_OUTPUT}")
