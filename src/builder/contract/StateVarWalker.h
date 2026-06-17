#pragma once

/// @file StateVarWalker.h
/// MRO-order iteration helpers (most-derived first; reverse for ctor init order).
/// Use `return;` in the lambda as `continue;`.

#include <libsolidity/ast/AST.h>

namespace puyasol::builder
{

/// Walk state variables, most-derived first.
template <typename F>
inline void forEachStateVar(
	solidity::frontend::ContractDefinition const& _contract,
	F&& _fn)
{
	for (auto const* base: _contract.annotation().linearizedBaseContracts)
		for (auto const* var: base->stateVariables())
			_fn(var);
}

/// Walk state variables, base-first (constructor/default-init order).
template <typename F>
inline void forEachStateVarReverse(
	solidity::frontend::ContractDefinition const& _contract,
	F&& _fn)
{
	auto const& lin = _contract.annotation().linearizedBaseContracts;
	for (auto it = lin.rbegin(); it != lin.rend(); ++it)
		for (auto const* var: (*it)->stateVariables())
			_fn(var);
}

/// Walk all defined functions, most-derived first.
template <typename F>
inline void forEachDefinedFunction(
	solidity::frontend::ContractDefinition const& _contract,
	F&& _fn)
{
	for (auto const* base: _contract.annotation().linearizedBaseContracts)
		for (auto const* func: base->definedFunctions())
			_fn(func);
}

/// Walk all function modifiers, most-derived first.
template <typename F>
inline void forEachFunctionModifier(
	solidity::frontend::ContractDefinition const& _contract,
	F&& _fn)
{
	for (auto const* base: _contract.annotation().linearizedBaseContracts)
		for (auto const* mod: base->functionModifiers())
			_fn(mod);
}

} // namespace puyasol::builder
