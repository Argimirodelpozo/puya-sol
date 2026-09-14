#pragma once

// Inline bodies here dynamic_cast to the concrete Type subclasses and read
// FunctionDefinition members, so this header needs the definitions.
#include <libsolidity/ast/AST.h>
#include <libsolidity/ast/Types.h>

#include <string>

namespace puyasol::builder
{

/// Appends a unique per-parameter-list suffix to `_name` so two overloads
/// of the same function name disambiguate by their parameter types.
///
/// Shape: `(t0,t1,...)` where `ti` is a short tag derived from the
/// parameter type:
///   - `BoolType`           → "b"
///   - `IntegerType`        → "u<bits>" or "i<bits>" (signed)
///   - `AddressType`        → "addr"
///   - `FixedBytesType`     → "b<N>"
///   - any other shape      → solc's `Type::identifier()` (signature-derived)
///
/// Two call sites need to agree on this shape: `FunctionBuilder.cpp` (when
/// building `method.memberName`) and `CallResolver.cpp` (when looking
/// up the resolved name). Sharing the implementation here prevents drift.
///
/// EVERY tag must be derived from the parameter's TYPE, never from AST
/// node ids of the declaration at hand: the naming side sees the contract's
/// most-derived override while a call inside an inherited base body sees
/// the base declaration (solc's referencedDeclaration is scope-relative).
/// The two FunctionDefinitions share a signature but not ids — a previous
/// fallback used `p->id()` and silently disagreed across that split for
/// overloads with non-value-typed params. `Type::identifier()` (e.g.
/// `t_string_memory_ptr`) is solc's stable signature-derived encoding;
/// struct/contract ids embedded in it refer to the TYPE's declaration,
/// which both views share.
///
/// We deliberately do NOT use solc's `Type::canonicalName()` for the value
/// cases. The short tags keep generated method names compact (saving TEAL
/// bytes for the bytecblock entries when methods are bytec-referenced); a
/// switch to canonical names would lengthen contract output by ~0.5–1 KB
/// on overload-heavy contracts (AAVE), with no corresponding readability
/// gain on the AVM-side.
inline void appendOverloadSuffix(
	std::string& _name,
	solidity::frontend::FunctionDefinition const& _func)
{
	_name += "(";
	bool first = true;
	for (auto const& p: _func.parameters())
	{
		if (!first) _name += ",";
		auto const* solType = p->type();
		if (dynamic_cast<solidity::frontend::BoolType const*>(solType))
			_name += "b";
		else if (auto const* intType = dynamic_cast<solidity::frontend::IntegerType const*>(solType))
			_name += (intType->isSigned() ? "i" : "u") + std::to_string(intType->numBits());
		else if (dynamic_cast<solidity::frontend::AddressType const*>(solType))
			_name += "addr";
		else if (auto const* fixedBytes = dynamic_cast<solidity::frontend::FixedBytesType const*>(solType))
			_name += "b" + std::to_string(fixedBytes->numBytes());
		else
			_name += solType->identifier();
		first = false;
	}
	_name += ")";
}

/// Returns the parameter-count-only suffix `(N)` for `_func`. A coarser
/// disambiguator than `appendOverloadSuffix` — sufficient where the
/// concern is "two functions with the same name but different arity"
/// rather than "two functions with the same name+arity but different
/// param types". Used by super-call resolution and a few name-keyed
/// caches where the registry layer separately tracks the param types.
inline std::string paramCountSuffix(solidity::frontend::FunctionDefinition const& _func)
{
	return "(" + std::to_string(_func.parameters().size()) + ")";
}

} // namespace puyasol::builder
