#pragma once

#include "builder/ast/SolExpression.h"

#include <libsolidity/ast/AST.h>
#include <functional>

namespace puyasol::builder::sol_ast
{

/// Base class for MemberAccess expression wrappers.
/// Provides access to the base expression, member name, and MemberAccess node.
class SolMemberAccess: public SolExpression
{
public:
	SolMemberAccess(
		eb::ContractContext& _ctx,
		solidity::frontend::MemberAccess const& _node);

	/// Evaluate a function-value projection without discarding receiver, option,
	/// or branch effects. The leaf callback may project metadata directly.
	static std::shared_ptr<awst::Expression> projectFunctionValue(
		eb::ContractContext& ctx, solidity::frontend::Expression const& source,
		awst::WType const* resultType, awst::SourceLocation const& loc,
		std::function<std::shared_ptr<awst::Expression>(solidity::frontend::Expression const&)> const& project);

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
