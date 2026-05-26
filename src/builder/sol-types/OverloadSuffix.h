#pragma once

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
///   - any other shape      → "<AST id>" (decimal, stable per param)
///
/// Two call sites need to agree on this shape: `FunctionBuilder.cpp` (when
/// building `method.memberName`) and `CallResolver.cpp` (when looking
/// up the resolved name). Sharing the implementation here prevents drift.
///
/// We deliberately do NOT use solc's `Type::canonicalName()` here. The
/// short tags keep generated method names compact (saving TEAL bytes for
/// the bytecblock entries when methods are bytec-referenced); a switch
/// to canonical names would lengthen contract output by ~0.5–1 KB on
/// overload-heavy contracts (AAVE), with no corresponding readability
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
			_name += std::to_string(p->id());
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
