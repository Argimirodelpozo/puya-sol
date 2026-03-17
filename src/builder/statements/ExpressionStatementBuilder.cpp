/// @file ExpressionStatementBuilder.cpp
/// Handles block and expression statement visitors.

#include "builder/statements/StatementBuilder.h"
#include "builder/assembly/AssemblyBuilder.h"
#include "builder/storage/StorageMapper.h"
#include "Logger.h"

namespace puyasol::builder
{

bool StatementBuilder::visit(solidity::frontend::Block const& _node)
{
	push(buildBlock(_node));
	return false;
}

bool StatementBuilder::visit(solidity::frontend::ExpressionStatement const& _node)
{
	auto loc = makeLoc(_node.location());
	auto expr = m_exprBuilder.build(_node.expression());

	// Flush pre-pending statements BEFORE the expression statement
	// (e.g., biguint exponentiation loops that compute values used by the expression)
	for (auto& pending: m_exprBuilder.takePrePendingStatements())
		push(std::move(pending));

	auto stmt = std::make_shared<awst::ExpressionStatement>();
	stmt->sourceLocation = loc;
	stmt->expr = std::move(expr);
	push(stmt);

	// Pick up any post-pending statements from the expression translator
	// (e.g., array length increment after push)
	for (auto& pending: m_exprBuilder.takePendingStatements())
		push(std::move(pending));

	return false;
}


} // namespace puyasol::builder
