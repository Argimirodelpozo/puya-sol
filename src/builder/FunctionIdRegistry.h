#pragma once

/// @file FunctionIdRegistry.h
/// First-pass routines AWSTBuilder runs before translating any
/// function body — they walk every source unit's library / free /
/// contract functions and:
///   1. Build the (qualifiedName → subroutineId) and (AST node id →
///      subroutineId) maps used downstream for resolving library /
///      operator-overload calls.
///   2. Preset the function-pointer dispatch cref to the first
///      deployable contract so that library subroutines can
///      construct SubroutineIDs before any contract is translated.
///
/// Extracted from AWSTBuilder.cpp — these two routines together
/// form a cohesive pre-pass and only mutate two map-typed out-
/// parameters (no other class state required).

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
