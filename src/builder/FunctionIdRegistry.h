#pragma once

/// @file FunctionIdRegistry.h
/// Pre-pass routines run by AWSTBuilder before translating any function body:
///   1. registerFunctionIds: build qualifiedName→subroutineId and AST-id→subroutineId
///      maps for resolving library / operator-overload calls.
///   2. presetDispatchCref: set the fn-ptr dispatch cref to the first deployable
///      contract so library subroutines can construct SubroutineIDs early.

#include "builder/contract/ContractBuilder.h"  // LibraryFunctionIdMap, FreeFunctionIdMap

#include <libsolidity/interface/CompilerStack.h>

#include <string>

namespace puyasol::builder
{

void registerFunctionIds(
	solidity::frontend::CompilerStack& _compiler,
	std::string const& _sourceFile,
	LibraryFunctionIdMap& _libraryFunctionIds,
	FreeFunctionIdMap& _freeFunctionById);

void presetDispatchCref(
	solidity::frontend::CompilerStack& _compiler,
	std::string const& _sourceFile);

} // namespace puyasol::builder
