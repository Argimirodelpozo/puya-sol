#pragma once

/// @file ParamABIValidator.h
/// Synthesises the entry-prologue statements that guard a function's
/// public/external parameters against the AVM's wider native types:
///
/// - Sub-64-bit unsigned ints: mask to the declared bit width. With
///   ABI coder v2, also assert `param <= maxValue` before masking
///   (silent truncation under v1).
/// - Sub-64-bit signed ints: under v2, assert
///   `param <= maxPos || param >= minNeg` (range check; no masking).
/// - bool params: under v2, assert `param <= 1`.
/// - Enum params: assert `param <= maxMember` (fires for both v1
///   and v2 — solc inlines the check at first-use for v1, we emit
///   at boundary for both as a strict superset).
///
/// EVM truncates ABI-decoded values to the declared type width
/// automatically; on AVM the uint64 native type preserves the full
/// value, so the runtime guard must be explicit. Returned statements
/// are inserted at the front of the function body by the caller.

#include "awst/Node.h"

#include <libsolidity/ast/AST.h>

#include <memory>
#include <string>
#include <vector>

namespace puyasol::builder
{

std::vector<std::shared_ptr<awst::Statement>> buildABIEntryChecks(
	solidity::frontend::FunctionDefinition const& _func,
	bool _useABICoderV2,
	std::string const& _sourceFile);

} // namespace puyasol::builder
