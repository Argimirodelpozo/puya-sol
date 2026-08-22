#pragma once

/// @file ParamABIValidator.h
/// Entry-prologue guards for public/external params. EVM auto-truncates ABI
/// values to the declared width; AVM uint64 preserves the full value, so the
/// check must be explicit:
///   - Sub-64-bit unsigned: mask (v2: assert ≤ maxVal first).
///   - Sub-64-bit signed: v2 assert param≤maxPos || param≥minNeg.
///   - bool: v2 assert param≤1.
///   - Enum: assert param≤maxMember (both v1 and v2 at boundary).

#include "awst/Node.h"

#include <libsolidity/ast/ASTForward.h>
#include "builder/sol-types/SolcFwd.h"

#include <memory>
#include <string>
#include <vector>

namespace puyasol::builder
{

class TypeMapper;

	std::vector<std::shared_ptr<awst::Statement>> buildABIEntryChecks(
	solidity::frontend::FunctionDefinition const& _func,
	TypeMapper const& _typeMapper,
	bool _useABICoderV2,
	std::string const& _sourceFile);

/// Resolved ABI parameter descriptor (type, AWST var name, source location).
struct ABIParamDesc
{
	solidity::frontend::Type const* solType;
	std::string name;
	awst::SourceLocation loc;
};

/// Same guards from explicit descriptors (no FunctionDefinition; for auto-
/// generated getters). `_enumChecksRequireV2`: auto-getters use enum keys
/// directly as mapping keys — under v1 there is no panic, so callers pass
/// true to suppress the enum check under v1. Sub-64-bit mask is unaffected.
std::vector<std::shared_ptr<awst::Statement>> buildABIEntryChecks(
	std::vector<ABIParamDesc> const& _params,
	bool _useABICoderV2,
	bool _enumChecksRequireV2 = false);

} // namespace puyasol::builder
