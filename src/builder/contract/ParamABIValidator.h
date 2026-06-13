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

/// A resolved ABI parameter: its Solidity type, the AWST variable name the
/// body reads it under (already non-empty), and its source location.
struct ABIParamDesc
{
	solidity::frontend::Type const* solType;
	std::string name;
	awst::SourceLocation loc;
};

/// Same entry guards from explicit descriptors — for synthesized methods
/// that have no FunctionDefinition (public-state-var getters, whose
/// sub-64-bit mapping-key params otherwise skip ABI validation: a raw
/// caller could pass an out-of-range key and hit the wrong storage slot).
///
/// `_enumChecksRequireV2`: gate the enum range-check on abicoder v2. A
/// user-written method that READS an enum param panics under both v1 and
/// v2 (default false → check always). But an AUTO-GENERATED getter uses
/// its enum key directly as a mapping key without "reading" it as an enum,
/// so under v1 it does NOT range-check (`table(0xa7)` returns 0, not a
/// panic) — getters pass true so the enum check only fires under v2. The
/// sub-64-bit mask/assert is unaffected (v1 truncates either way).
std::vector<std::shared_ptr<awst::Statement>> buildABIEntryChecks(
	std::vector<ABIParamDesc> const& _params,
	bool _useABICoderV2,
	bool _enumChecksRequireV2 = false);

} // namespace puyasol::builder
