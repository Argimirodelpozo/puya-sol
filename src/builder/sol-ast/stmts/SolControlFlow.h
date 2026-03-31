#pragma once

#include "builder/sol-ast/SolStatement.h"

namespace puyasol::builder::sol_ast
{

class SolIfStatement: public SolStatement
{
public:
	SolIfStatement(StatementContext& _ctx,
		solidity::frontend::IfStatement const& _node, awst::SourceLocation _loc);
	std::vector<std::shared_ptr<awst::Statement>> toAwst() override;
private:
	solidity::frontend::IfStatement const& m_node;
};

class SolWhileStatement: public SolStatement
{
public:
	SolWhileStatement(StatementContext& _ctx,
		solidity::frontend::WhileStatement const& _node, awst::SourceLocation _loc);
	std::vector<std::shared_ptr<awst::Statement>> toAwst() override;
private:
	solidity::frontend::WhileStatement const& m_node;
};

class SolForStatement: public SolStatement
{
public:
	SolForStatement(StatementContext& _ctx,
		solidity::frontend::ForStatement const& _node, awst::SourceLocation _loc);
	std::vector<std::shared_ptr<awst::Statement>> toAwst() override;
private:
	solidity::frontend::ForStatement const& m_node;
};

} // namespace puyasol::builder::sol_ast
