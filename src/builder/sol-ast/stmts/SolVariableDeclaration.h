#pragma once

#include "builder/sol-ast/SolStatement.h"

namespace puyasol::builder::sol_ast
{

/// Variable declaration: type name = expr; or (type1 a, type2 b) = expr;
/// Handles initializers, storage aliases, function pointers, constant tracking,
/// new-array size upgrading, and tuple destructuring.
class SolVariableDeclaration: public SolStatement
{
public:
	SolVariableDeclaration(BlockContext& _blk,
		solidity::frontend::VariableDeclarationStatement const& _node,
		awst::SourceLocation _loc);
	std::vector<std::shared_ptr<awst::Statement>> toAwst() override;

private:
	solidity::frontend::VariableDeclarationStatement const& m_node;
};

} // namespace puyasol::builder::sol_ast
