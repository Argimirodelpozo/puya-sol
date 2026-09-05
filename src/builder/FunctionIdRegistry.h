#pragma once

/// @file FunctionIdRegistry.h
/// Pre-pass routines run by AWSTBuilder before translating any function body:
///   1. registerFunctionIds: build opaque declaration-ID symbols for root
///      free/library subroutines and contract-local internal/private methods.
///   2. presetDispatchCref: set the fn-ptr dispatch cref to the first deployable
///      contract so library subroutines can construct SubroutineIDs early.

#include "builder/FunctionSymbolTable.h"

#include <libsolidity/interface/CompilerStack.h>

#include <string>

namespace puyasol::builder
{

namespace eb { struct FunctionPointerRegistry; }

void registerFunctionIds(
	solidity::frontend::CompilerStack& _compiler,
	FunctionSymbolTable& _functionSymbols);

void presetDispatchCref(
	solidity::frontend::CompilerStack& _compiler,
	eb::FunctionPointerRegistry& _functionPointers);

} // namespace puyasol::builder
