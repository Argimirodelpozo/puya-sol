#pragma once

/// @file StateVarWalker.h
/// Tiny inline helpers that iterate a contract's state variables in
/// linearized-base-contract (MRO) order. Replaces ~25 hand-rolled
/// double-for loops scattered across builder/, mostly in
/// ApprovalProgramBuilder.cpp (14+ sites).
///
/// The MRO order matters: forward walks (most-derived first) match
/// Solidity's storage layout / public-getter precedence; reverse
/// walks (base-first) match the constructor-initialiser and default-
/// init order. Pick the matching helper accordingly.

#include <libsolidity/ast/AST.h>

namespace puyasol::builder
{

/// Walk all state variables of `_contract` in MRO order
/// (most-derived first). `_fn` receives a
/// `VariableDeclaration const*`, matching the pointer form used by
/// the hand-rolled double-for loops it replaces — minimises edit
/// surface at call sites. Use `return;` inside the lambda for the
/// `continue;` equivalent.
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
/// ordering. `_fn` receives a `VariableDeclaration const*`.
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

} // namespace puyasol::builder
