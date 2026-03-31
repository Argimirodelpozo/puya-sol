#pragma once

#include "builder/sol-eb/BuilderContext.h"
#include "builder/sol-ast/SolFunctionCall.h"
#include "builder/sol-ast/SolMemberAccess.h"

#include <libsolidity/ast/AST.h>

#include <memory>

namespace puyasol::builder::sol_ast
{

/// Creates the appropriate SolExpression subclass for a Solidity AST node.
///
/// Uses FunctionCallKind, FunctionType::Kind, and Type::Category to select
/// the right concrete class. Returns nullptr for nodes not yet migrated
/// (caller falls through to legacy code).
class SolExpressionFactory
{
public:
	explicit SolExpressionFactory(eb::BuilderContext& _ctx);

	/// Create a SolFunctionCall for a FunctionCall AST node.
	/// Returns nullptr if the call kind is not yet handled.
	std::unique_ptr<SolFunctionCall> createFunctionCall(
		solidity::frontend::FunctionCall const& _node);

	/// Create a SolMemberAccess for a MemberAccess AST node.
	/// Returns nullptr if the member access kind is not yet handled.
	std::unique_ptr<SolMemberAccess> createMemberAccess(
		solidity::frontend::MemberAccess const& _node);

private:
	eb::BuilderContext& m_ctx;
};

} // namespace puyasol::builder::sol_ast
