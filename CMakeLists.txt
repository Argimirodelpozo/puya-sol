cmake_minimum_required(VERSION 3.13)
project(puya-sol VERSION 0.1.0 LANGUAGES CXX)

include(CTest)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Solidity source and build directories
set(SOLIDITY_DIR "${CMAKE_SOURCE_DIR}/solidity")
set(SOLIDITY_BUILD_DIR "${SOLIDITY_DIR}/build")

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

# Boost (required by libsolidity)
set(BOOST_ROOT "$ENV{HOME}/.local/boost-1.83" CACHE PATH "Boost root")
set(Boost_NO_SYSTEM_PATHS ON)
set(Boost_NO_BOOST_CMAKE ON)
find_package(Boost REQUIRED COMPONENTS filesystem)
include_directories(${Boost_INCLUDE_DIRS})

# Source files
set(SOURCES
	src/main.cpp
	src/cli/CliOptions.cpp
	src/cli/SourceCompat.cpp
	src/cli/CompilerSetup.cpp
	src/cli/AwstPostPasses.cpp
	src/Logger.cpp
	src/awst/WType.cpp
	src/awst/Node.cpp
	src/awst/Visit.cpp
	src/awst/Clone.cpp
	src/awst/Termination.cpp
	# Builder core
	src/builder/AWSTBuilder.cpp
	src/builder/ProgramAnalysis.cpp
	src/builder/ParameterMutationAnalysis.cpp
	src/builder/SolcFacts.cpp
	src/builder/SourceLocConvert.cpp
	src/builder/FunctionIdRegistry.cpp
	src/builder/SubroutineRegistry.cpp
	src/builder/contract/ContractBuilder.cpp
	src/builder/contract/StorageDispatch.cpp
	src/builder/contract/EvmSlotStorageDispatch.cpp
	src/builder/contract/ModifierBodyInliner.cpp
	src/builder/contract/ModifierInliner.cpp
	src/builder/contract/FunctionBuilder.cpp
	src/builder/contract/ApprovalProgramBuilder.cpp
	src/builder/contract/ParamABIValidator.cpp
	src/builder/contract/PostInitTriggers.cpp
	src/builder/contract/PublicGetterBuilder.cpp
	src/builder/contract/ReturnRewriter.cpp
	src/builder/contract/SelectorRouter.cpp
	src/builder/contract/SuperCallResolution.cpp

	# Builtin support (algorithms with no AVM opcode counterpart)
	src/builder/builtin/Ripemd160Builder.cpp

	# Assembly builder (split by operation type)
	src/builder/assembly/AssemblyBuilder.cpp
	src/builder/assembly/CoreTranslation.cpp
	src/builder/assembly/ArithmeticOps.cpp
	src/builder/assembly/BitwiseShiftOps.cpp
	src/builder/assembly/CalldataMapOps.cpp
	src/builder/assembly/ControlFlowOps.cpp
	src/builder/assembly/SignedOps.cpp
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
	src/builder/storage/StorageMapper.cpp
	src/builder/storage/StoragePlace.cpp
	src/builder/storage/StorageLayout.cpp
	src/builder/storage/StorageRuntimePlan.cpp
	src/builder/storage/SlotHandleAccess.cpp
	src/builder/storage/SlotWordCodec.cpp
	src/builder/storage/TransientStorage.cpp
	src/builder/sol-types/TypeMapper.cpp
	src/builder/sol-intrinsics/IntrinsicMapper.cpp

	# Expression builder helpers (sol-eb/)
	src/builder/abi/AbiSelectorCalldataBuilder.cpp
	src/builder/sol-eb/BigUIntMathHelpers.cpp
	src/builder/sol-eb/BinaryOpBuilder.cpp
	src/builder/sol-eb/ContractContext.cpp
	src/builder/sol-eb/BuiltinCallables.cpp
	src/builder/itxn/InnerCallHandlers.cpp
	src/builder/itxn/InnerCallShapes.cpp
	src/builder/sol-eb/NodeBuilder.cpp
	src/builder/sol-eb/SolArrayBuilder.cpp
	src/builder/sol-eb/SolBoolBuilder.cpp
	src/builder/sol-eb/SolIntegerBuilder.cpp
	src/builder/sol-eb/SolStringBuilder.cpp
	src/builder/sol-eb/SolStructBuilder.cpp
	src/builder/sol-eb/SolAddressBuilder.cpp
	src/builder/sol-eb/SolEnumBuilder.cpp
	src/builder/sol-eb/SolFixedBytesBuilder.cpp
	src/builder/sol-eb/BuilderRegistry.cpp
	src/builder/itxn/CallResolver.cpp
	src/builder/itxn/AsaIntrinsics.cpp
	src/builder/storage/StorageBackend.cpp
	src/builder/abi/AbiEncoderBuilder.cpp
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
	src/builder/sol-ast/exprs/SolAssignment.cpp
	src/builder/sol-ast/exprs/SolAssignmentEarlyOuts.cpp
	src/builder/sol-ast/exprs/SolAssignmentTuple.cpp
	src/builder/sol-ast/exprs/SolAssignmentStructField.cpp
	src/builder/sol-ast/exprs/SolAssignmentBytesElem.cpp
	src/builder/sol-ast/SolStatement.cpp
	src/builder/sol-ast/stmts/SolExpressionStatement.cpp
	src/builder/sol-ast/stmts/SolControlFlow.cpp
	src/builder/sol-ast/stmts/SolEmitStatement.cpp
	src/builder/sol-ast/stmts/SolVariableDeclaration.cpp
	src/builder/sol-ast/stmts/SolInlineAssembly.cpp
	src/builder/sol-ast/stmts/SolBlock.cpp
	src/json/AWSTSerializer.cpp
	src/json/Base85.cpp
	src/json/OptionsWriter.cpp
	src/runner/PuyaRunner.cpp
	src/experimental/splitter/FunctionSplitter.cpp
	src/experimental/splitter/PureHelperExtractor.cpp
	src/experimental/splitter/AwstWalker.cpp
	src/experimental/splitter/SimpleSplitter.cpp
	src/experimental/splitter/SimpleSplitterRunner.cpp
)

add_executable(puya-sol ${SOURCES})

# A dangling-reference bug in the solc AST wrapper survived because normal
# builds suppress warnings. Keep this high-signal lifetime diagnostic fatal.
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
	target_compile_options(puya-sol PRIVATE -Werror=return-local-addr)
endif()

# Link solidity static libraries
target_link_libraries(puya-sol
	${SOLIDITY_BUILD_DIR}/libsolidity/libsolidity.a
	${SOLIDITY_BUILD_DIR}/libyul/libyul.a
	${SOLIDITY_BUILD_DIR}/libevmasm/libevmasm.a
	${SOLIDITY_BUILD_DIR}/liblangutil/liblangutil.a
	${SOLIDITY_BUILD_DIR}/libsmtutil/libsmtutil.a
	${SOLIDITY_BUILD_DIR}/libsolutil/libsolutil.a
	${Boost_LIBRARIES}
	pthread
)

# z3 optional
find_library(Z3_LIB z3)
if(Z3_LIB)
	target_link_libraries(puya-sol ${Z3_LIB})
endif()

if(BUILD_TESTING)
	add_executable(puya-sol-runner-tests
		tests/cpp/PuyaRunnerTests.cpp
		src/runner/PuyaRunner.cpp
		src/Logger.cpp
	)
	target_include_directories(puya-sol-runner-tests PRIVATE ${CMAKE_SOURCE_DIR}/src)
	add_test(NAME puya-sol-runner-paths COMMAND puya-sol-runner-tests)

	add_executable(puya-sol-source-compat-tests
		tests/cpp/SourceCompatTests.cpp
		src/cli/SourceCompat.cpp
		src/Logger.cpp
	)
	target_include_directories(puya-sol-source-compat-tests PRIVATE ${CMAKE_SOURCE_DIR}/src)
	target_link_libraries(puya-sol-source-compat-tests
		${SOLIDITY_BUILD_DIR}/liblangutil/liblangutil.a
		${SOLIDITY_BUILD_DIR}/libsolutil/libsolutil.a
		${Boost_LIBRARIES}
		pthread
	)
	add_test(NAME puya-sol-source-compat COMMAND puya-sol-source-compat-tests)

	add_executable(puya-sol-wtype-tests
		tests/cpp/WTypeTests.cpp
		src/awst/WType.cpp
	)
	target_include_directories(puya-sol-wtype-tests PRIVATE ${CMAKE_SOURCE_DIR}/src)
	add_test(NAME puya-sol-wtype-equivalence COMMAND puya-sol-wtype-tests)

	add_executable(puya-sol-scratch-layout-tests
		tests/cpp/ScratchLayoutTests.cpp
	)
	target_include_directories(puya-sol-scratch-layout-tests PRIVATE ${CMAKE_SOURCE_DIR}/src)
	add_test(NAME puya-sol-scratch-layout COMMAND puya-sol-scratch-layout-tests)

	add_executable(puya-sol-function-symbol-table-tests
		tests/cpp/FunctionSymbolTableTests.cpp
	)
	target_include_directories(puya-sol-function-symbol-table-tests PRIVATE ${CMAKE_SOURCE_DIR}/src)
	add_test(NAME puya-sol-function-symbol-table COMMAND puya-sol-function-symbol-table-tests)
endif()
