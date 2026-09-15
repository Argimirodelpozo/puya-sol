#pragma once

#include <libsolidity/ast/AST.h>

#include <optional>
#include <string>

namespace puyasol::builder
{

inline bool isRootSubroutine(solidity::frontend::FunctionDefinition const& function)
{
	auto const* owner = function.annotation().contract;
	return function.isFree() || (owner && owner->isLibrary());
}

/// Opaque AWST identity derived from solc; public contract methods retain their
/// ABI names. No registration order or mutable symbol cache participates.
inline std::optional<std::string> functionSymbol(solidity::frontend::FunctionDefinition const& function)
{
	using solidity::frontend::Visibility;
	if (!function.isImplemented() || function.isConstructor()) return {};
	if (isRootSubroutine(function) || function.visibility() == Visibility::Internal
		|| function.visibility() == Visibility::Private)
		return "__solfn_" + std::to_string(function.id());
	return {};
}

} // namespace puyasol::builder
