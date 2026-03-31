#pragma once

#include "builder/sol-ast/SolExpression.h"

#include <libsolidity/ast/AST.h>

namespace puyasol::builder::sol_ast
{

/// Base class for MemberAccess expression wrappers.
/// Provides access to the base expression, member name, and MemberAccess node.
class SolMemberAccess: public SolExpression
{
public:
	SolMemberAccess(
		eb::BuilderContext& _ctx,
		solidity::frontend::MemberAccess const& _node);

	/// The underlying MemberAccess AST node.
	solidity::frontend::MemberAccess const& memberAccess() const { return m_memberAccess; }

	/// The member name being accessed.
	std::string const& memberName() const { return m_memberAccess.memberName(); }

	/// The base expression (what's before the dot).
	solidity::frontend::Expression const& baseExpression() const
	{
		return m_memberAccess.expression();
	}

protected:
	solidity::frontend::MemberAccess const& m_memberAccess;
};

} // namespace puyasol::builder::sol_ast
