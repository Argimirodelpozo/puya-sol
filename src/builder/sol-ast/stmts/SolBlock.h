#pragma once

#include "builder/sol-ast/SolStatement.h"

namespace puyasol::builder::sol_ast
{

/// Block statement: { stmt1; stmt2; ... }
/// Runs child statements under a nested BlockContext; unchecked-block flag
/// is mutated for the duration and restored by toAwstBlock.
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
