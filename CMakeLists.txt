cmake_minimum_required(VERSION 3.13)
project(puya-sol VERSION 0.1.0 LANGUAGES CXX)

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
find_package(Boost REQUIRED COMPONENTS filesystem)
include_directories(${Boost_INCLUDE_DIRS})

# Source files
set(SOURCES
	src/main.cpp
	src/Logger.cpp
	src/awst/WType.cpp
	src/awst/Node.cpp
	# Builder core
	src/builder/AWSTBuilder.cpp
	src/builder/ContractBuilder.cpp

	# Expression builder (split by expression type)
	src/builder/expressions/ExpressionBuilder.cpp
	src/builder/expressions/LiteralBuilder.cpp
	src/builder/expressions/IdentifierBuilder.cpp
	src/builder/expressions/BinaryOperationBuilder.cpp
	src/builder/expressions/UnaryOperationBuilder.cpp
	src/builder/expressions/FunctionCallBuilder.cpp
	src/builder/expressions/MemberAccessBuilder.cpp
	src/builder/expressions/IndexAccessBuilder.cpp
	src/builder/expressions/ConditionalBuilder.cpp
	src/builder/expressions/AssignmentBuilder.cpp
	src/builder/expressions/TupleBuilder.cpp
	src/builder/expressions/HelpersBuilder.cpp

	# Statement builder (split by statement type)
	src/builder/statements/StatementBuilder.cpp
	src/builder/statements/ExpressionStatementBuilder.cpp
	src/builder/statements/ReturnBuilder.cpp
	src/builder/statements/ControlFlowBuilder.cpp
	src/builder/statements/VariableDeclarationBuilder.cpp
	src/builder/statements/EmitBuilder.cpp
	src/builder/statements/RevertBuilder.cpp
	src/builder/statements/InlineAssemblyBuilder.cpp

	# Assembly builder (split by operation type)
	src/builder/assembly/AssemblyBuilder.cpp
	src/builder/assembly/CoreTranslation.cpp
	src/builder/assembly/ArithmeticOps.cpp
	src/builder/assembly/BitwiseShiftOps.cpp
	src/builder/assembly/SignedOps.cpp
	src/builder/assembly/DataOps.cpp
	src/builder/assembly/MemoryHelpers.cpp
	src/builder/assembly/PrecompileDispatch.cpp
	src/builder/assembly/PrecompileHandlers.cpp
	src/builder/assembly/MemoryOps.cpp
	src/builder/assembly/StatementOps.cpp

	# Storage, type, and intrinsic mappers
	src/builder/storage/StorageMapper.cpp
	src/builder/sol-types/TypeMapper.cpp
	src/builder/sol-intrinsics/IntrinsicMapper.cpp
	src/json/AWSTSerializer.cpp
	src/json/Base85.cpp
	src/json/OptionsWriter.cpp
	src/runner/PuyaRunner.cpp
	src/splitter/SizeEstimator.cpp
	src/splitter/CallGraphAnalyzer.cpp
	src/splitter/ContractSplitter.cpp
	src/splitter/FunctionSplitter.cpp
	src/splitter/FunctionInliner.cpp
	src/splitter/ConstantExternalizer.cpp
)

add_executable(puya-sol ${SOURCES})

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
