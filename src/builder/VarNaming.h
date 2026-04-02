#pragma once

#include <libsolidity/ast/AST.h>
#include <string>

namespace puyasol::builder
{

/// Generate a unique variable name from a Solidity VariableDeclaration.
/// Uses name_declId to prevent shadowing conflicts in nested scopes.
/// e.g., `uint x` with decl ID 42 → "x_42"
inline std::string uniqueVarName(solidity::frontend::VariableDeclaration const& _decl)
{
	return _decl.name() + "_" + std::to_string(_decl.id());
}

} // namespace puyasol::builder
