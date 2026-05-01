#pragma once

#include "builder/sol-ast/SolStatement.h"

namespace puyasol::builder::sol_ast
{

/// Block statement: { stmt1; stmt2; ... }
/// Holds the per-block scope: child statements run with a derived
/// BlockContext (nested), and unchecked-block flag mutates BuilderContext
/// for the duration (RAII via a guard inside toAwstBlock).
class SolBlock: public SolStatement
{
public:
	SolBlock(BlockContext& _blk,
		solidity::frontend::Block const& _node,
		awst::SourceLocation _loc);

	std::vector<std::shared_ptr<awst::Statement>> toAwst() override;

	/// Build as an awst::Block (the primary public API).
	std::shared_ptr<awst::Block> toAwstBlock();

private:
	solidity::frontend::Block const& m_block;
};

} // namespace puyasol::builder::sol_ast
