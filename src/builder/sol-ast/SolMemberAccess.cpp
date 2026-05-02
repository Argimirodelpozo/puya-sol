#include "builder/sol-ast/SolMemberAccess.h"

namespace puyasol::builder::sol_ast
{

SolMemberAccess::SolMemberAccess(
	eb::ContractContext& _ctx,
	solidity::frontend::MemberAccess const& _node)
	: SolExpression(_ctx, _node),
	  m_memberAccess(_node)
{
}

} // namespace puyasol::builder::sol_ast
