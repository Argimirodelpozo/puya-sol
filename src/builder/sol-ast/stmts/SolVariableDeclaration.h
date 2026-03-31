#pragma once

#include "builder/sol-ast/SolStatement.h"

namespace puyasol::builder
{
class ExpressionBuilder;
}

namespace puyasol::builder::sol_ast
{

/// Variable declaration: type name = expr; or (type1 a, type2 b) = expr;
/// Handles initializers, storage aliases, function pointers, constant tracking,
/// new-array size upgrading, and tuple destructuring.
class SolVariableDeclaration: public SolStatement
{
public:
	SolVariableDeclaration(StatementContext& _ctx,
		solidity::frontend::VariableDeclarationStatement const& _node,
		awst::SourceLocation _loc,
		ExpressionBuilder& _exprBuilder);
	std::vector<std::shared_ptr<awst::Statement>> toAwst() override;

private:
	solidity::frontend::VariableDeclarationStatement const& m_node;
	ExpressionBuilder& m_exprBuilder;
};

} // namespace puyasol::builder::sol_ast
