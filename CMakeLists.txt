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
	src/builder/AWSTBuilder.cpp
	src/builder/TypeMapper.cpp
	src/builder/ExpressionTranslator.cpp
	src/builder/StatementTranslator.cpp
	src/builder/ContractTranslator.cpp
	src/builder/StorageMapper.cpp
	src/builder/IntrinsicMapper.cpp
	src/builder/AssemblyTranslator.cpp
	src/json/AWSTSerializer.cpp
	src/json/Base85.cpp
	src/json/OptionsWriter.cpp
	src/runner/PuyaRunner.cpp
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
