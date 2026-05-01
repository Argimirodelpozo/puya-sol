#pragma once

#include "builder/sol-ast/SolStatement.h"

namespace puyasol::builder::sol_ast
{

/// Expression statement: expr;
/// Flushes pre/post pending statements from expression evaluation.
class SolExpressionStatement: public SolStatement
{
public:
	SolExpressionStatement(BlockContext& _blk,
		solidity::frontend::ExpressionStatement const& _node,
		awst::SourceLocation _loc);
	std::vector<std::shared_ptr<awst::Statement>> toAwst() override;

private:
	solidity::frontend::ExpressionStatement const& m_node;
};

/// Revert statement: revert Error();
class SolRevertStatement: public SolStatement
{
public:
	SolRevertStatement(BlockContext& _blk,
		solidity::frontend::RevertStatement const& _node,
		awst::SourceLocation _loc);
	std::vector<std::shared_ptr<awst::Statement>> toAwst() override;

private:
	solidity::frontend::RevertStatement const& m_node;
};

/// Return statement with type coercion.
class SolReturnStatement: public SolStatement
{
public:
	SolReturnStatement(BlockContext& _blk,
		solidity::frontend::Return const& _node,
		awst::SourceLocation _loc);
	std::vector<std::shared_ptr<awst::Statement>> toAwst() override;

private:
	solidity::frontend::Return const& m_node;
};

} // namespace puyasol::builder::sol_ast
