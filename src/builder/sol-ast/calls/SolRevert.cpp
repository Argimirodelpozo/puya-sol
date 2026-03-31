#include "builder/sol-ast/calls/SolRevert.h"

namespace puyasol::builder::sol_ast
{

SolRevert::SolRevert(
	eb::BuilderContext& _ctx,
	solidity::frontend::FunctionCall const& _call)
	: SolFunctionCall(_ctx, _call)
{
}

std::shared_ptr<awst::Expression> SolRevert::toAwst()
{
	auto assertExpr = std::make_shared<awst::AssertExpression>();
	assertExpr->sourceLocation = m_loc;
	assertExpr->wtype = awst::WType::voidType();

	auto falseLit = std::make_shared<awst::BoolConstant>();
	falseLit->sourceLocation = m_loc;
	falseLit->wtype = awst::WType::boolType();
	falseLit->value = false;
	assertExpr->condition = std::move(falseLit);

	if (!m_call.arguments().empty())
	{
		if (auto const* lit = dynamic_cast<solidity::frontend::Literal const*>(
				m_call.arguments()[0].get()))
			assertExpr->errorMessage = lit->value();
		else
		{
			// Custom error: extract name from the expression
			auto const& errExpr = m_call.arguments()[0];
			if (auto const* errCall = dynamic_cast<solidity::frontend::FunctionCall const*>(errExpr.get()))
			{
				auto const& callee = errCall->expression();
				if (auto const* ma = dynamic_cast<solidity::frontend::MemberAccess const*>(&callee))
					assertExpr->errorMessage = ma->memberName();
				else if (auto const* id = dynamic_cast<solidity::frontend::Identifier const*>(&callee))
					assertExpr->errorMessage = id->name();
				else
					assertExpr->errorMessage = "revert";
			}
			else
				assertExpr->errorMessage = "revert";
		}
	}
	else
		assertExpr->errorMessage = "revert";

	return assertExpr;
}

} // namespace puyasol::builder::sol_ast
