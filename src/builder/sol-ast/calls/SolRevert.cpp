#include "builder/sol-ast/calls/SolRevert.h"
#include "builder/sol-ast/calls/RevertBlob.h"

#include <libsolidity/ast/AST.h>
#include <libsolidity/ast/ASTAnnotations.h>

namespace puyasol::builder::sol_ast
{

SolRevert::SolRevert(
	eb::ContractContext& _ctx,
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
	std::shared_ptr<awst::Expression> revertBlob;
	auto const& callee = m_call.expression();
	if (auto const* id = dynamic_cast<solidity::frontend::Identifier const*>(&callee))
	{
		if (id->name() != "revert")
			errorMessage = id->name();
		else if (!m_call.arguments().empty())
		{
			// `revert("msg")` / `revert(stringExpr)` — log the EVM-shaped
			// Error(string) payload before failing so the revert reason is
			// client-readable (via simulate). Bare `revert()` keeps empty
			// revert data, matching EVM.
			auto msgExpr = buildExpr(*m_call.arguments()[0]);
			if (auto const* sc = dynamic_cast<awst::StringConstant const*>(msgExpr.get()))
			{
				errorMessage = sc->value;
				revertBlob = awst::makeBytesConstant(
					errorStringRevertBlobBytes(sc->value), m_loc);
			}
			else
				revertBlob = makeErrorStringRevertBlob(std::move(msgExpr), m_loc);
		}
	}
	else if (auto const* ma = dynamic_cast<solidity::frontend::MemberAccess const*>(&callee))
	{
		// Custom error `revert E(args)` — name as the TEAL comment; the
		// selector+args payload is a follow-up.
		errorMessage = ma->memberName();
	}

	if (revertBlob)
		m_ctx.prePendingStatements.push_back(
			makeRevertLogStmt(std::move(revertBlob), m_loc));

	return awst::makeAssert(awst::makeFalse(m_loc), m_loc, std::move(errorMessage));
}

} // namespace puyasol::builder::sol_ast
