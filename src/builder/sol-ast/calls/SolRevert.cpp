#include "builder/sol-ast/calls/SolRevert.h"

#include <libsolidity/ast/ASTAnnotations.h>

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

	// Determine error message. For `revert Error(args)`, the callee
	// identifies the error name. For `revert("msg")`, Solidity treats
	// this as a FunctionCall whose callee is the identifier `revert`,
	// and the first argument is the message literal.
	assertExpr->errorMessage = "revert";
	auto const& callee = m_call.expression();
	if (auto const* id = dynamic_cast<solidity::frontend::Identifier const*>(&callee))
	{
		if (id->name() != "revert")
			assertExpr->errorMessage = id->name();
	}
	else if (auto const* ma = dynamic_cast<solidity::frontend::MemberAccess const*>(&callee))
	{
		assertExpr->errorMessage = ma->memberName();
	}

	return assertExpr;
}

} // namespace puyasol::builder::sol_ast
