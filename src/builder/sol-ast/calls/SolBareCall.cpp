#include "builder/sol-ast/calls/SolBareCall.h"
#include "builder/itxn/InnerCallHandlers.h"

namespace puyasol::builder::sol_ast
{

std::shared_ptr<awst::Expression> SolBareCall::toAwst()
{
	auto const& funcExpr = funcExpression();
	auto const* memberAccess = dynamic_cast<solidity::frontend::MemberAccess const*>(&funcExpr);
	if (!memberAccess)
	{
		auto vc = awst::makeVoidConstant(m_loc);
		return vc;
	}

	auto receiver = buildExpr(memberAccess->expression());

	auto result = eb::InnerCallHandlers::tryHandleAddressCall(
		m_ctx, receiver, memberAccess->memberName(),
		m_call, extractCallValue(), memberAccess->expression(), m_loc);
	if (result)
		return result->resolve();

	// Fallback: return (true, empty bytes) tuple
	auto vc = awst::makeVoidConstant(m_loc);
	return vc;
}

} // namespace puyasol::builder::sol_ast
