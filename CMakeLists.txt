cmake_minimum_required(VERSION 3.16)
project(puya-sol VERSION 0.1.0 LANGUAGES C CXX)

include(CTest)
include(ExternalProject)
include(GNUInstallDirs)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
	set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING
		"Build type (Debug, Release, RelWithDebInfo, or MinSizeRel)" FORCE)
	set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS
		Debug Release RelWithDebInfo MinSizeRel)
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Reuse object files across rebuilds and across branches. Opt-in: this only
# engages if ccache is on PATH, so the build works unchanged without it.
find_program(CCACHE_PROGRAM ccache)
if(CCACHE_PROGRAM)
	set(CMAKE_CXX_COMPILER_LAUNCHER "${CCACHE_PROGRAM}" CACHE STRING "" FORCE)
	set(CMAKE_C_COMPILER_LAUNCHER "${CCACHE_PROGRAM}" CACHE STRING "" FORCE)
	message(STATUS "ccache enabled: ${CCACHE_PROGRAM}")
else()
	message(STATUS "ccache not found — install ccache to enable it")
endif()

# Solidity is built as a separate top-level CMake project because its CMake
# modules intentionally use CMAKE_SOURCE_DIR. Keeping its binary tree under the
# puya-sol binary directory prevents ignored or stale artifacts in the source
# submodule from being linked accidentally.
set(SOLIDITY_DIR "${CMAKE_SOURCE_DIR}/solidity")
set(SOLIDITY_BUILD_DIR "${CMAKE_BINARY_DIR}/solidity")

include("${CMAKE_SOURCE_DIR}/cmake/VerifySolidityCheckout.cmake")
puya_sol_verify_solidity_checkout(
	"${CMAKE_SOURCE_DIR}"
	"${SOLIDITY_DIR}"
	SOLIDITY_COMMIT
)
message(STATUS "Pinned Solidity revision: ${SOLIDITY_COMMIT}")

# Solidity requires Boost 1.83 or newer. Use normal CMake discovery instead of
# a developer-specific path. When a root hint is supplied, prevent FindBoost
# from combining headers from that root with incompatible system libraries.
set(Boost_NO_BOOST_CMAKE ON CACHE BOOL "Use CMake's FindBoost module")
set(Boost_NO_WARN_NEW_VERSIONS ON CACHE BOOL
	"Suppress FindBoost warnings for supported Boost releases")
set(Boost_USE_STATIC_LIBS ON CACHE BOOL "Link Boost statically")
if(NOT DEFINED Boost_NO_SYSTEM_PATHS)
	if((DEFINED BOOST_ROOT AND NOT BOOST_ROOT STREQUAL "")
		OR (DEFINED Boost_ROOT AND NOT Boost_ROOT STREQUAL "")
		OR (DEFINED ENV{BOOST_ROOT} AND NOT "$ENV{BOOST_ROOT}" STREQUAL ""))
		set(Boost_NO_SYSTEM_PATHS ON CACHE BOOL
			"Only search the explicitly selected Boost root")
	else()
		set(Boost_NO_SYSTEM_PATHS OFF CACHE BOOL
			"Allow normal system Boost discovery")
	endif()
endif()
find_package(Boost 1.83 REQUIRED COMPONENTS
	filesystem
	program_options
	unit_test_framework
)
find_package(Threads REQUIRED)

set(SOLIDITY_ARCHIVES
	"${SOLIDITY_BUILD_DIR}/libsolidity/${CMAKE_STATIC_LIBRARY_PREFIX}solidity${CMAKE_STATIC_LIBRARY_SUFFIX}"
	"${SOLIDITY_BUILD_DIR}/libyul/${CMAKE_STATIC_LIBRARY_PREFIX}yul${CMAKE_STATIC_LIBRARY_SUFFIX}"
	"${SOLIDITY_BUILD_DIR}/libevmasm/${CMAKE_STATIC_LIBRARY_PREFIX}evmasm${CMAKE_STATIC_LIBRARY_SUFFIX}"
	"${SOLIDITY_BUILD_DIR}/liblangutil/${CMAKE_STATIC_LIBRARY_PREFIX}langutil${CMAKE_STATIC_LIBRARY_SUFFIX}"
	"${SOLIDITY_BUILD_DIR}/libsmtutil/${CMAKE_STATIC_LIBRARY_PREFIX}smtutil${CMAKE_STATIC_LIBRARY_SUFFIX}"
	"${SOLIDITY_BUILD_DIR}/libsolutil/${CMAKE_STATIC_LIBRARY_PREFIX}solutil${CMAKE_STATIC_LIBRARY_SUFFIX}"
)

set(SOLIDITY_CMAKE_CACHE_ARGS
	"-DCMAKE_BUILD_TYPE:STRING=${CMAKE_BUILD_TYPE}"
	"-DCMAKE_C_COMPILER:FILEPATH=${CMAKE_C_COMPILER}"
	"-DCMAKE_CXX_COMPILER:FILEPATH=${CMAKE_CXX_COMPILER}"
	"-DONLY_BUILD_SOLIDITY_LIBRARIES:BOOL=ON"
	"-DTESTS:BOOL=OFF"
	"-DTOOLS:BOOL=OFF"
	"-DPEDANTIC:BOOL=ON"
	"-DBoost_NO_BOOST_CMAKE:BOOL=${Boost_NO_BOOST_CMAKE}"
	"-DBoost_NO_SYSTEM_PATHS:BOOL=${Boost_NO_SYSTEM_PATHS}"
	"-DBoost_NO_WARN_NEW_VERSIONS:BOOL=${Boost_NO_WARN_NEW_VERSIONS}"
	"-DBoost_USE_STATIC_LIBS:BOOL=${Boost_USE_STATIC_LIBS}"
	"-DBoost_INCLUDE_DIR:PATH=${Boost_INCLUDE_DIR}"
)
foreach(boost_var IN ITEMS
	BOOST_ROOT
	Boost_ROOT
	Boost_LIBRARY_DIR_DEBUG
	Boost_LIBRARY_DIR_RELEASE
	Boost_FILESYSTEM_LIBRARY_DEBUG
	Boost_FILESYSTEM_LIBRARY_RELEASE
	Boost_PROGRAM_OPTIONS_LIBRARY_DEBUG
	Boost_PROGRAM_OPTIONS_LIBRARY_RELEASE
	Boost_UNIT_TEST_FRAMEWORK_LIBRARY_DEBUG
	Boost_UNIT_TEST_FRAMEWORK_LIBRARY_RELEASE
)
	if(DEFINED ${boost_var} AND NOT "${${boost_var}}" STREQUAL ""
		AND NOT "${${boost_var}}" MATCHES "NOTFOUND$")
		list(APPEND SOLIDITY_CMAKE_CACHE_ARGS
			"-D${boost_var}:PATH=${${boost_var}}")
	endif()
endforeach()
if(CMAKE_C_COMPILER_LAUNCHER)
	list(APPEND SOLIDITY_CMAKE_CACHE_ARGS
		"-DCMAKE_C_COMPILER_LAUNCHER:FILEPATH=${CMAKE_C_COMPILER_LAUNCHER}")
endif()
if(CMAKE_CXX_COMPILER_LAUNCHER)
	list(APPEND SOLIDITY_CMAKE_CACHE_ARGS
		"-DCMAKE_CXX_COMPILER_LAUNCHER:FILEPATH=${CMAKE_CXX_COMPILER_LAUNCHER}")
endif()

add_custom_target(puya-sol-verify-solidity-checkout
	COMMAND "${CMAKE_COMMAND}"
		"-DPUYA_SOL_ROOT_DIR=${CMAKE_SOURCE_DIR}"
		"-DPUYA_SOL_SOLIDITY_DIR=${SOLIDITY_DIR}"
		-P "${CMAKE_SOURCE_DIR}/cmake/VerifySolidityCheckout.cmake"
	COMMENT "Verifying pinned Solidity checkout"
	VERBATIM
)

ExternalProject_Add(puya-sol-solidity
	PREFIX "${CMAKE_BINARY_DIR}/_deps/solidity-subbuild"
	SOURCE_DIR "${SOLIDITY_DIR}"
	BINARY_DIR "${SOLIDITY_BUILD_DIR}"
	DOWNLOAD_COMMAND ""
	UPDATE_COMMAND ""
	PATCH_COMMAND ""
	CMAKE_CACHE_ARGS ${SOLIDITY_CMAKE_CACHE_ARGS}
	INSTALL_COMMAND ""
	BUILD_ALWAYS ON
	BUILD_BYPRODUCTS ${SOLIDITY_ARCHIVES}
	USES_TERMINAL_CONFIGURE ON
	USES_TERMINAL_BUILD ON
)
add_dependencies(puya-sol-solidity puya-sol-verify-solidity-checkout)

add_custom_target(puya-sol-verify-solidity-build
	COMMAND "${CMAKE_COMMAND}"
		"-DPUYA_SOL_SOLIDITY_BUILD_DIR=${SOLIDITY_BUILD_DIR}"
		"-DPUYA_SOL_SOLIDITY_COMMIT=${SOLIDITY_COMMIT}"
		"-DPUYA_SOL_STATIC_LIBRARY_PREFIX=${CMAKE_STATIC_LIBRARY_PREFIX}"
		"-DPUYA_SOL_STATIC_LIBRARY_SUFFIX=${CMAKE_STATIC_LIBRARY_SUFFIX}"
		-P "${CMAKE_SOURCE_DIR}/cmake/VerifySolidityBuild.cmake"
	DEPENDS puya-sol-solidity
	COMMENT "Verifying pinned Solidity build identity"
	VERBATIM
)

set(solidity_library_index 0)
foreach(solidity_library IN ITEMS solidity yul evmasm langutil smtutil solutil)
	list(GET SOLIDITY_ARCHIVES ${solidity_library_index} solidity_archive)
	set(solidity_target "puya_sol_${solidity_library}")
	add_library(${solidity_target} STATIC IMPORTED GLOBAL)
	set_target_properties(${solidity_target} PROPERTIES
		IMPORTED_LOCATION "${solidity_archive}"
	)
	math(EXPR solidity_library_index "${solidity_library_index} + 1")
endforeach()

# Include paths
include_directories(
	${CMAKE_SOURCE_DIR}/src
	${SOLIDITY_DIR}
	${SOLIDITY_DIR}/deps/nlohmann-json/include
	${SOLIDITY_DIR}/deps/fmtlib/include
	${SOLIDITY_DIR}/deps/range-v3/include
	${SOLIDITY_BUILD_DIR}/include
)

# fmt is header-only in solidity
add_definitions(-DFMT_HEADER_ONLY=1)
include_directories(${Boost_INCLUDE_DIRS})

# Source files
set(SOURCES
	src/main.cpp
	src/ArtifactIO.cpp
	src/cli/CliOptions.cpp
	src/cli/SourceCompat.cpp
	src/cli/CompilerSetup.cpp
	src/cli/AwstPostPasses.cpp
	src/Logger.cpp
	src/awst/WType.cpp
	src/awst/Node.cpp
	src/awst/Visit.cpp
	# Builder core
	src/builder/AWSTBuilder.cpp
	src/builder/CompilationSession.cpp
	src/builder/EvmFeaturePolicy.cpp
	src/builder/ProgramAnalysis.cpp
	src/builder/ReturnWirePlan.cpp
	src/builder/CallBoundaryPlan.cpp
	src/builder/ParameterMutationAnalysis.cpp
	src/builder/SolcFacts.cpp
	src/builder/SelectorSemantics.cpp
	src/builder/SourceLocConvert.cpp
	src/builder/FunctionIdRegistry.cpp
	src/builder/contract/ContractBuilder.cpp
	src/builder/contract/EvmMemoryCodec.cpp
	src/builder/contract/EvmEntryRouter.cpp
	src/builder/contract/StorageDispatch.cpp
	src/builder/contract/EvmSlotStorageDispatch.cpp
	src/builder/contract/ModifierChainBuilder.cpp
	src/builder/contract/FunctionBuilder.cpp
	src/builder/contract/ApprovalProgramBuilder.cpp
	src/builder/contract/ParamABIValidator.cpp
	src/builder/contract/PostInitTriggers.cpp
	src/builder/contract/PostInitBuilder.cpp
	src/builder/contract/PublicGetterBuilder.cpp
	src/builder/contract/SelectorRouter.cpp
	src/builder/contract/SuperCallResolution.cpp

	# Builtin support (algorithms with no AVM opcode counterpart)
	src/builder/builtin/AppCodeSizeLowering.cpp
	src/builder/builtin/Ripemd160Builder.cpp

	# Assembly builder (split by operation type)
	src/builder/assembly/AssemblyBuilder.cpp
	src/builder/assembly/CoreTranslation.cpp
	src/builder/assembly/ArithmeticOps.cpp
	src/builder/assembly/BitwiseShiftOps.cpp
	src/builder/assembly/CalldataMapOps.cpp
	src/builder/assembly/ControlFlowOps.cpp
	src/builder/assembly/SignedOps.cpp
	src/builder/assembly/StorageOps.cpp
	src/builder/assembly/DataOps.cpp
	src/builder/assembly/MemoryHelpers.cpp
	src/builder/assembly/PrecompileDispatch.cpp
	src/builder/assembly/PrecompileHandlers.cpp
	src/builder/assembly/MemoryOps.cpp
	src/builder/assembly/StatementOps.cpp
	src/builder/assembly/SyntheticCalldataOps.cpp
	src/builder/assembly/UserFunctionOps.cpp

	# Type coercion utilities
	src/builder/sol-types/Arc4ArrayWidening.cpp
	src/builder/sol-types/Arc4Defaults.cpp
	src/builder/sol-types/SolIntType.cpp
	src/builder/sol-types/SolcConstFold.cpp
	src/builder/sol-types/TypeCoercion.cpp
	src/builder/sol-types/ConversionPlan.cpp

	# Storage, type, and intrinsic mappers
	src/builder/proxies/Erc1967Lowering.cpp
	src/builder/proxies/UupsLowering.cpp
	src/builder/storage/StorageMapper.cpp
	src/builder/storage/StorageLayout.cpp
	src/builder/storage/StorageRuntimePlan.cpp
	src/builder/storage/SlotHandleAccess.cpp
	src/builder/storage/SlotWordCodec.cpp
	src/builder/storage/TransientStorage.cpp
	src/builder/sol-types/TypeMapper.cpp
	src/builder/sol-intrinsics/IntrinsicMapper.cpp

	# Expression builder helpers (sol-eb/)
	src/builder/abi/AbiSelectorCalldataBuilder.cpp
	src/builder/abi/Arc4Stdlib.cpp
	src/builder/sol-eb/BigUIntMathHelpers.cpp
	src/builder/sol-eb/BinaryOpBuilder.cpp
	src/builder/sol-eb/ContractContext.cpp
	src/builder/sol-eb/BuiltinCallables.cpp
	src/builder/itxn/InnerCallHandlers.cpp
	src/builder/itxn/InnerCallShapes.cpp
	src/builder/itxn/NativePayment.cpp
	src/builder/sol-eb/NodeBuilder.cpp
	src/builder/sol-eb/SolArrayBuilder.cpp
	src/builder/sol-eb/SolBoolBuilder.cpp
	src/builder/sol-eb/SolIntegerBuilder.cpp
	src/builder/sol-eb/SolStructBuilder.cpp
	src/builder/sol-eb/SolAddressBuilder.cpp
	src/builder/sol-eb/SolEnumBuilder.cpp
	src/builder/sol-eb/SolFixedBytesBuilder.cpp
	src/builder/sol-eb/BuilderRegistry.cpp
	src/builder/itxn/CallResolver.cpp
	src/builder/itxn/AsaIntrinsics.cpp
	src/builder/storage/StorageBackend.cpp
	src/builder/abi/AbiEncoderBuilder.cpp
	src/builder/abi/EvmAbiEncode.cpp
	src/builder/abi/EvmAbiDecode.cpp
	src/builder/codec/EvmValueCodec.cpp
	src/builder/itxn/FunctionPointerBuilder.cpp
	src/builder/itxn/FunctionPointerDispatchTypes.cpp
	src/builder/sol-eb/AssignmentHelper.cpp
	src/builder/sol-eb/TypeConversions.cpp
	src/builder/sol-ast/Context.cpp
	src/builder/sol-ast/EvmSlotLowering.cpp
	src/builder/sol-ast/EvmSlotValueLowering.cpp
	src/builder/sol-ast/SolExpression.cpp
	src/builder/sol-ast/SolFunctionCall.cpp
	src/builder/sol-ast/SolExpressionFactory.cpp
	src/builder/sol-ast/SolExpressionDispatch.cpp
	src/builder/sol-ast/calls/SolRequireAssert.cpp
	src/builder/sol-ast/calls/SolRevert.cpp
	src/builder/sol-ast/calls/SolBuiltinCall.cpp
	src/builder/sol-ast/calls/SolTypeConversion.cpp
	src/builder/sol-ast/calls/SolWrapUnwrap.cpp
	src/builder/sol-ast/calls/SolStructConstruction.cpp
	src/builder/sol-ast/calls/SolTransferSend.cpp
	src/builder/sol-ast/calls/SolBareCall.cpp
	src/builder/sol-ast/calls/SolAbiEncode.cpp
	src/builder/sol-ast/calls/SolAbiDecode.cpp
	src/builder/sol-ast/calls/SolArrayMethod.cpp
	src/builder/sol-ast/calls/SolArrayMethodHandlers.cpp
	src/builder/sol-ast/calls/SolInternalCall.cpp
	src/builder/sol-ast/EffectScan.cpp
	src/builder/sol-ast/calls/SolExternalCall.cpp
	src/builder/sol-ast/calls/SolNewExpression.cpp
	src/builder/sol-ast/calls/SolBytesConcat.cpp
	src/builder/sol-ast/calls/SolMetaType.cpp
	src/builder/sol-ast/SolMemberAccess.cpp
	src/builder/sol-ast/members/SolIntrinsicAccess.cpp
	src/builder/sol-ast/members/SolEnumValueAccess.cpp
	src/builder/sol-ast/members/SolSelectorAccess.cpp
	src/builder/sol-ast/members/SolMetaTypeAccess.cpp
	src/builder/sol-ast/members/SolLengthAccess.cpp
	src/builder/sol-ast/members/SolFieldAccess.cpp
	src/builder/sol-ast/members/SolAddressProperty.cpp
	src/builder/sol-ast/members/SolConstantAccess.cpp
	src/builder/sol-ast/exprs/SolLiteral.cpp
	src/builder/sol-ast/exprs/SolConditional.cpp
	src/builder/sol-ast/exprs/SolIdentifier.cpp
	src/builder/sol-ast/exprs/SolTupleExpression.cpp
	src/builder/sol-ast/exprs/SolBinaryOperation.cpp
	src/builder/sol-ast/exprs/SolUnaryOperation.cpp
	src/builder/sol-ast/exprs/SolIndexAccess.cpp
	src/builder/sol-ast/exprs/SolIndexAccessHandlers.cpp
	src/builder/sol-ast/MappingPrefix.cpp
	src/builder/sol-ast/exprs/SolAssignment.cpp
	src/builder/sol-ast/exprs/SolAssignmentEarlyOuts.cpp
	src/builder/sol-ast/exprs/SolAssignmentTuple.cpp
	src/builder/sol-ast/exprs/SolAssignmentStructField.cpp
	src/builder/sol-ast/exprs/SolAssignmentBytesElem.cpp
	src/builder/sol-ast/stmts/SolExpressionStatement.cpp
	src/builder/sol-ast/stmts/SolControlFlow.cpp
	src/builder/sol-ast/stmts/SolEmitStatement.cpp
	src/builder/sol-ast/stmts/SolVariableDeclaration.cpp
	src/builder/sol-ast/stmts/SolInlineAssembly.cpp
	src/builder/sol-ast/stmts/SolBlock.cpp
	src/json/AWSTSerializer.cpp
	src/json/OptionsWriter.cpp
	src/runner/PuyaRunner.cpp
)

add_executable(puya-sol ${SOURCES})

set(PUYA_SOL_STDLIB_SOURCE "${CMAKE_SOURCE_DIR}/src/libs/AVM.sol")
set(PUYA_SOL_STDLIB_DESTINATION
	"${CMAKE_BINARY_DIR}/${CMAKE_INSTALL_DATADIR}/puya-sol/libs/AVM.sol")
add_custom_command(
	OUTPUT "${PUYA_SOL_STDLIB_DESTINATION}"
	COMMAND "${CMAKE_COMMAND}" -E make_directory
		"${CMAKE_BINARY_DIR}/${CMAKE_INSTALL_DATADIR}/puya-sol/libs"
	COMMAND "${CMAKE_COMMAND}" -E copy_if_different
		"${PUYA_SOL_STDLIB_SOURCE}"
		"${PUYA_SOL_STDLIB_DESTINATION}"
	DEPENDS "${PUYA_SOL_STDLIB_SOURCE}"
	COMMENT "Staging puya-sol Solidity standard library"
	VERBATIM
)
add_custom_target(puya-sol-stdlib DEPENDS "${PUYA_SOL_STDLIB_DESTINATION}")
add_dependencies(puya-sol
	puya-sol-stdlib
	puya-sol-verify-solidity-build
)

# Precompile only what nearly EVERY translation unit already needs.
#
# Deliberately NOT libsolidity/ast/AST.h or Types.h: a PCH is force-included
# into every source, so listing those would push them back into the ~60 TUs
# that were just decoupled from them via builder/sol-types/SolcFwd.h. awst/Node.h
# is different — it is already in 134 of 139 builder TUs and pulls no
# third-party headers, so precompiling it is pure win with no re-coupling.
target_precompile_headers(puya-sol PRIVATE
	"${CMAKE_SOURCE_DIR}/src/awst/Node.h"
	<map>
	<memory>
	<optional>
	<string>
	<variant>
	<vector>
)

# A dangling-reference bug in the solc AST wrapper survived because normal
# builds suppress warnings. Keep this high-signal lifetime diagnostic fatal.
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
	target_compile_options(puya-sol PRIVATE -Werror=return-local-addr)
endif()

# Link the archives produced by the pinned, CMake-managed Solidity build. Keep
# dependency order from the top-level Solidity target for static linking.
target_link_libraries(puya-sol
	puya_sol_solidity
	puya_sol_yul
	puya_sol_evmasm
	puya_sol_langutil
	puya_sol_smtutil
	puya_sol_solutil
	Boost::filesystem
	Threads::Threads
)

install(TARGETS puya-sol RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
install(FILES "${PUYA_SOL_STDLIB_SOURCE}"
	DESTINATION "${CMAKE_INSTALL_DATADIR}/puya-sol/libs")

set(PUYA_SOL_BUILD_MANIFEST "${CMAKE_BINARY_DIR}/puya-sol-build-manifest.txt")
add_custom_target(puya-sol-build-manifest ALL
	COMMAND "${CMAKE_COMMAND}"
		"-DPUYA_SOL_ROOT_DIR=${CMAKE_SOURCE_DIR}"
		"-DPUYA_SOL_BINARY=$<TARGET_FILE:puya-sol>"
		"-DPUYA_SOL_OUTPUT=${PUYA_SOL_BUILD_MANIFEST}"
		"-DPUYA_SOL_SOLIDITY_COMMIT=${SOLIDITY_COMMIT}"
		"-DPUYA_SOL_STDLIB=${PUYA_SOL_STDLIB_SOURCE}"
		"-DPUYA_SOL_CXX_COMPILER=${CMAKE_CXX_COMPILER}"
		"-DPUYA_SOL_BUILD_TYPE=${CMAKE_BUILD_TYPE}"
		"-DPUYA_SOL_BOOST_VERSION=${Boost_VERSION_STRING}"
		"-DPUYA_SOL_BOOST_INCLUDE_DIR=${Boost_INCLUDE_DIR}"
		"-DPUYA_SOL_BOOST_FILESYSTEM_LIBRARY=${Boost_FILESYSTEM_LIBRARY_RELEASE}"
		-P "${CMAKE_SOURCE_DIR}/cmake/WriteBuildManifest.cmake"
	DEPENDS puya-sol "${CMAKE_SOURCE_DIR}/cmake/WriteBuildManifest.cmake"
	BYPRODUCTS "${PUYA_SOL_BUILD_MANIFEST}"
	COMMENT "Recording build provenance"
	VERBATIM
)

if(BUILD_TESTING)
	add_executable(puya-sol-termination-tests
		tests/cpp/TerminationTests.cpp
		src/awst/WType.cpp
	)
	target_include_directories(puya-sol-termination-tests PRIVATE ${CMAKE_SOURCE_DIR}/src)
	add_test(NAME puya-sol-termination COMMAND puya-sol-termination-tests)
	add_test(NAME puya-sol-source-identity
		COMMAND ${CMAKE_COMMAND}
			-DPUYA_SOL=$<TARGET_FILE:puya-sol>
			-DOUTPUT_ROOT=${CMAKE_BINARY_DIR}/source-identity-test-output
			-P ${CMAKE_SOURCE_DIR}/tests/cmake/SourceIdentityTest.cmake)

	add_executable(puya-sol-runner-tests
		tests/cpp/PuyaRunnerTests.cpp
		src/runner/PuyaRunner.cpp
		src/Logger.cpp
	)
	target_include_directories(puya-sol-runner-tests PRIVATE ${CMAKE_SOURCE_DIR}/src)
	add_test(NAME puya-sol-runner-paths COMMAND puya-sol-runner-tests)

	add_executable(puya-sol-source-compat-tests
		tests/cpp/SourceCompatTests.cpp
		src/ArtifactIO.cpp
		src/cli/SourceCompat.cpp
		src/Logger.cpp
	)
	target_include_directories(puya-sol-source-compat-tests PRIVATE ${CMAKE_SOURCE_DIR}/src)
	add_dependencies(puya-sol-source-compat-tests puya-sol-verify-solidity-build)
	target_link_libraries(puya-sol-source-compat-tests
		puya_sol_langutil
		puya_sol_solutil
		Boost::filesystem
		Threads::Threads
	)
	add_test(NAME puya-sol-source-compat COMMAND puya-sol-source-compat-tests)

	add_executable(puya-sol-wtype-tests
		tests/cpp/WTypeTests.cpp
		src/awst/WType.cpp
	)
	target_include_directories(puya-sol-wtype-tests PRIVATE ${CMAKE_SOURCE_DIR}/src)
	add_test(NAME puya-sol-wtype-equivalence COMMAND puya-sol-wtype-tests)

	add_executable(puya-sol-encoded-size-tests
		tests/cpp/EncodedSizeTests.cpp
		src/builder/sol-types/Arc4Defaults.cpp
		src/awst/WType.cpp
	)
	target_include_directories(puya-sol-encoded-size-tests PRIVATE ${CMAKE_SOURCE_DIR}/src)
	add_test(NAME puya-sol-encoded-size COMMAND puya-sol-encoded-size-tests)
	add_test(NAME puya-sol-encoded-size-diagnostics
		COMMAND ${CMAKE_COMMAND}
			-DPUYA_SOL=$<TARGET_FILE:puya-sol>
			-DSOURCE_DIR=${CMAKE_SOURCE_DIR}/tests/cli-validation
			-DOUTPUT_ROOT=${CMAKE_BINARY_DIR}/encoded-size-test-output
			-P ${CMAKE_SOURCE_DIR}/tests/cmake/EncodedSizeTest.cmake)

	add_executable(puya-sol-scratch-layout-tests
		tests/cpp/ScratchLayoutTests.cpp
	)
	target_include_directories(puya-sol-scratch-layout-tests PRIVATE ${CMAKE_SOURCE_DIR}/src)
	add_test(NAME puya-sol-scratch-layout COMMAND puya-sol-scratch-layout-tests)
	add_test(
		NAME puya-sol-cli-memory-slots
		COMMAND ${CMAKE_COMMAND}
			-DPUYA_SOL=$<TARGET_FILE:puya-sol>
			-P ${CMAKE_SOURCE_DIR}/tests/cmake/CliMemorySlotsTest.cmake
	)
	add_test(
		NAME puya-sol-cli-divergence-policy
		COMMAND ${CMAKE_COMMAND}
			-DPUYA_SOL=$<TARGET_FILE:puya-sol>
			-DSOURCE_DIR=${CMAKE_SOURCE_DIR}/tests/cli-validation
			-DOUTPUT_ROOT=${CMAKE_BINARY_DIR}/divergence-policy-test-output
			-P ${CMAKE_SOURCE_DIR}/tests/cmake/DivergencePolicyTest.cmake
	)
	add_test(
		NAME puya-sol-cli-xchain-validation
		COMMAND ${CMAKE_COMMAND}
			-DPUYA_SOL=$<TARGET_FILE:puya-sol>
			-DSOURCE_FILE=${CMAKE_SOURCE_DIR}/tests/cli-validation/Minimal.sol
			-DOUTPUT_ROOT=${CMAKE_BINARY_DIR}/xchain-cli-test-output
			-P ${CMAKE_SOURCE_DIR}/tests/cmake/XchainCliValidationTest.cmake
	)
	add_test(
		NAME puya-sol-cli-removed-options
		COMMAND ${CMAKE_COMMAND}
			-DPUYA_SOL=$<TARGET_FILE:puya-sol>
			-P ${CMAKE_SOURCE_DIR}/tests/cmake/RemovedCliOptionsTest.cmake
	)
	add_test(
		NAME puya-sol-cli-source-policy
		COMMAND ${CMAKE_COMMAND}
			-DPUYA_SOL=$<TARGET_FILE:puya-sol>
			-DSOURCE_DIR=${CMAKE_SOURCE_DIR}/tests/cli-validation
			-DOUTPUT_ROOT=${CMAKE_BINARY_DIR}/source-policy-test-output
			-P ${CMAKE_SOURCE_DIR}/tests/cmake/SourcePolicyTest.cmake
	)
	add_test(
		NAME puya-sol-cli-evm-layout-options
		COMMAND ${CMAKE_COMMAND}
			-DPUYA_SOL=$<TARGET_FILE:puya-sol>
			-DSOURCE_FILE=${CMAKE_SOURCE_DIR}/tests/cli-validation/Minimal.sol
			-DOUTPUT_ROOT=${CMAKE_BINARY_DIR}/evm-layout-options-test-output
			-P ${CMAKE_SOURCE_DIR}/tests/cmake/EvmLayoutOptionsTest.cmake
	)

	add_executable(puya-sol-function-symbol-table-tests
		tests/cpp/FunctionSymbolTableTests.cpp
	)
	target_include_directories(puya-sol-function-symbol-table-tests PRIVATE ${CMAKE_SOURCE_DIR}/src)
	add_test(NAME puya-sol-function-symbol-table COMMAND puya-sol-function-symbol-table-tests)

	add_executable(puya-sol-hex-bytes-tests
		tests/cpp/HexBytesTests.cpp
	)
	target_include_directories(puya-sol-hex-bytes-tests PRIVATE ${CMAKE_SOURCE_DIR}/src)
	add_test(NAME puya-sol-hex-bytes COMMAND puya-sol-hex-bytes-tests)

	add_executable(puya-sol-artifact-writer-tests
		tests/cpp/ArtifactWriterTests.cpp
		src/ArtifactIO.cpp
		src/awst/WType.cpp
		src/cli/AwstPostPasses.cpp
		src/json/OptionsWriter.cpp
		src/Logger.cpp
	)
	target_include_directories(puya-sol-artifact-writer-tests PRIVATE
		${CMAKE_SOURCE_DIR}/src)
	add_dependencies(puya-sol-artifact-writer-tests
		puya-sol-verify-solidity-build)
	target_link_libraries(puya-sol-artifact-writer-tests
		puya_sol_solutil
		Boost::filesystem
		Threads::Threads
	)
	add_test(NAME puya-sol-artifact-writers
		COMMAND puya-sol-artifact-writer-tests)

	add_test(
		NAME puya-sol-arc4-stdlib
		COMMAND ${CMAKE_COMMAND}
			-DPUYA_SOL=$<TARGET_FILE:puya-sol>
			-DSOURCE_DIR=${CMAKE_SOURCE_DIR}/tests/arc4-stdlib
			-DOUTPUT_ROOT=${CMAKE_BINARY_DIR}/arc4-stdlib-test-output
			-P ${CMAKE_SOURCE_DIR}/tests/cmake/Arc4StdlibTest.cmake
	)
endif()
