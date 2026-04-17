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
	// Determine error message. For `revert Error(args)`, the callee
	// identifies the error name. For `revert("msg")`, Solidity treats
	// this as a FunctionCall whose callee is the identifier `revert`,
	// and the first argument is the message literal.
	std::string errorMessage = "revert";
	auto const& callee = m_call.expression();
	if (auto const* id = dynamic_cast<solidity::frontend::Identifier const*>(&callee))
	{
		if (id->name() != "revert")
			errorMessage = id->name();
	}
	else if (auto const* ma = dynamic_cast<solidity::frontend::MemberAccess const*>(&callee))
	{
		errorMessage = ma->memberName();
	}

	return awst::makeAssert(awst::makeBoolConstant(false, m_loc), m_loc, std::move(errorMessage));
}

} // namespace puyasol::builder::sol_ast
