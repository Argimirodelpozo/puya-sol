#pragma once

#include "builder/sol-ast/SolStatement.h"

namespace puyasol::builder::sol_ast
{

/// emit Event(args...) → ARC-28 event log.
class SolEmitStatement: public SolStatement
{
public:
	SolEmitStatement(BlockContext& _blk,
		solidity::frontend::EmitStatement const& _node, awst::SourceLocation _loc);
	std::vector<std::shared_ptr<awst::Statement>> toAwst() override;
private:
	solidity::frontend::EmitStatement const& m_node;
};

} // namespace puyasol::builder::sol_ast
