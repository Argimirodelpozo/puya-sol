#pragma once

/// @file FunctionIdRegistry.h
/// Pre-pass routines run by AWSTBuilder before translating any function body:
///   1. registerFunctionIds: build opaque declaration-ID symbols for root
///      free/library subroutines and contract-local internal/private methods.
///   2. presetDispatchCref: set the fn-ptr dispatch cref to the first deployable
///      contract so library subroutines can construct SubroutineIDs early.

#include "builder/context/FunctionSymbolTable.h"

namespace puyasol::builder
{

namespace eb { struct FunctionPointerRegistry; }
struct ProgramAnalysis;

void registerFunctionIds(
	ProgramAnalysis const& _analysis,
	FunctionSymbolTable& _functionSymbols);

void presetDispatchCref(
	ProgramAnalysis const& _analysis,
	eb::FunctionPointerRegistry& _functionPointers);

} // namespace puyasol::builder
