#pragma once

/// @file StateVarWalker.h
/// Tiny inline helpers that iterate a contract's state variables /
/// defined functions / function modifiers in linearized-base-contract
/// (MRO) order. Replaces hand-rolled double-for loops scattered across
/// builder/.
///
/// The MRO order matters: forward walks (most-derived first) match
/// Solidity's storage layout / public-getter precedence; reverse
/// walks (base-first) match the constructor-initialiser and default-
/// init order. Pick the matching helper accordingly.
///
/// Each helper passes the inner element to `_fn` as a `const*` —
/// matching the pointer form used by the hand-rolled loops being
/// replaced, so the body conversion is mostly mechanical. Use
/// `return;` inside the lambda for the `continue;` equivalent.

#include <libsolidity/ast/AST.h>

namespace puyasol::builder
{

/// Walk all state variables of `_contract` in MRO order
/// (most-derived first). `_fn` receives a
/// `VariableDeclaration const*`.
template <typename F>
inline void forEachStateVar(
	solidity::frontend::ContractDefinition const& _contract,
	F&& _fn)
{
	for (auto const* base: _contract.annotation().linearizedBaseContracts)
		for (auto const* var: base->stateVariables())
			_fn(var);
}

/// Walk all state variables of `_contract` in reverse MRO order
/// (base-first). Matches constructor-initialiser / default-init
/// ordering.
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

/// Walk all functions defined in `_contract`'s MRO chain
/// (most-derived first). `_fn` receives a
/// `FunctionDefinition const*`. Used wherever the builder enumerates
/// every callable a contract exposes (selector tables, dispatch
/// routing, super-target lookup).
template <typename F>
inline void forEachDefinedFunction(
	solidity::frontend::ContractDefinition const& _contract,
	F&& _fn)
{
	for (auto const* base: _contract.annotation().linearizedBaseContracts)
		for (auto const* func: base->definedFunctions())
			_fn(func);
}

/// Walk all modifiers defined in `_contract`'s MRO chain
/// (most-derived first). `_fn` receives a
/// `ModifierDefinition const*`. Used by the modifier-inliner to
/// resolve `modifier_name` references against the full inheritance
/// chain.
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
