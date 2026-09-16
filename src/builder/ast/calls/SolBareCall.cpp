#include "builder/ast/calls/SolBareCall.h"
#include "builder/solc/SolcFacts.h"
#include "builder/lowering/itxn/InnerCallHandlers.h"
#include "builder/AwstShorthand.h"

namespace puyasol::builder::sol_ast
{

std::shared_ptr<awst::Expression> SolBareCall::toAwst()
{
	auto const& funcExpr = funcExpression();
	auto const* memberAccess = SolcFacts::expressionAs<solidity::frontend::MemberAccess>(&funcExpr);
	if (!memberAccess)
	{
		auto vc = awst::makeVoidConstant(m_loc);
		return vc;
	}

	auto operand = m_ctx.lower(memberAccess->expression(), false);
	bool const pin = !shorthand::isCurrentAppAddressGlobal(operand.value.get());
	auto receiver = m_ctx.emitSequencedOperand(
		std::move(operand.effects), std::move(operand.value), pin, m_loc);

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
