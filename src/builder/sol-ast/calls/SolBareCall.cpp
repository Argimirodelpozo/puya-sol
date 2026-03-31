#include "builder/sol-ast/calls/SolBareCall.h"
#include "builder/sol-eb/InnerCallHandlers.h"

namespace puyasol::builder::sol_ast
{

std::shared_ptr<awst::Expression> SolBareCall::toAwst()
{
	auto const& funcExpr = funcExpression();
	auto const* memberAccess = dynamic_cast<solidity::frontend::MemberAccess const*>(&funcExpr);
	if (!memberAccess)
	{
		auto vc = std::make_shared<awst::VoidConstant>();
		vc->sourceLocation = m_loc;
		vc->wtype = awst::WType::voidType();
		return vc;
	}

	auto receiver = buildExpr(memberAccess->expression());

	auto result = eb::InnerCallHandlers::tryHandleAddressCall(
		m_ctx, receiver, memberAccess->memberName(),
		m_call, extractCallValue(), memberAccess->expression(), m_loc);
	if (result)
		return result->resolve();

	// Fallback: return (true, empty bytes) tuple
	auto vc = std::make_shared<awst::VoidConstant>();
	vc->sourceLocation = m_loc;
	vc->wtype = awst::WType::voidType();
	return vc;
}

} // namespace puyasol::builder::sol_ast
