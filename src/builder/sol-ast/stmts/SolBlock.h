#pragma once

#include "builder/sol-ast/SolStatement.h"

namespace puyasol::builder
{
class ExpressionBuilder;
}

namespace puyasol::builder::sol_ast
{

/// Block statement: { stmt1; stmt2; ... }
/// This is the central entry point for building AWST from Solidity statements.
/// Handles unchecked blocks, nested block flattening, and dispatches each
/// statement to the appropriate sol-ast wrapper class.
class SolBlock: public SolStatement
{
public:
	SolBlock(StatementContext& _ctx,
		solidity::frontend::Block const& _node,
		awst::SourceLocation _loc,
		ExpressionBuilder& _exprBuilder);

	std::vector<std::shared_ptr<awst::Statement>> toAwst() override;

	/// Build as an awst::Block (the primary public API).
	std::shared_ptr<awst::Block> toAwstBlock();

private:
	solidity::frontend::Block const& m_block;
	ExpressionBuilder& m_exprBuilder;
};

/// Build a single Solidity statement into AWST (convenience free function).
/// Dispatches to the right sol-ast wrapper and wraps multiple results in a Block.
std::shared_ptr<awst::Statement> buildStatement(
	StatementContext& _ctx,
	ExpressionBuilder& _exprBuilder,
	solidity::frontend::Statement const& _stmt);

/// Build a Solidity Block into an AWST Block (convenience free function).
std::shared_ptr<awst::Block> buildBlock(
	StatementContext& _ctx,
	ExpressionBuilder& _exprBuilder,
	solidity::frontend::Block const& _block);

} // namespace puyasol::builder::sol_ast
