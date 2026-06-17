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
	// `revert Error(args)`: callee identifies error name.
	// `revert("msg")`: FunctionCall with identifier `revert`; first arg is message.
	std::string errorMessage = "revert";
	std::shared_ptr<awst::Expression> revertBlob;
	auto const& callee = m_call.expression();
	if (auto const* id = dynamic_cast<solidity::frontend::Identifier const*>(&callee))
	{
		if (id->name() != "revert")
			errorMessage = id->name();
		else if (!m_call.arguments().empty())
		{
			// Log Error(string) payload for client readability (simulate).
			// Bare `revert()` → empty revert data (EVM-compatible).
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
		// Custom error: name as TEAL comment; selector+args follow-up.
		errorMessage = ma->memberName();
	}

	auto failNode = awst::makeAssert(
		awst::makeFalse(m_loc), m_loc, std::move(errorMessage));
	if (revertBlob)
	{
		m_ctx.prePendingStatements.push_back(
			makeRevertLogStmt(std::move(revertBlob), m_loc));
		// isExplicit=false: let puya optimizer strip provably-unreachable fail.
		failNode->isExplicit = false;
	}
	return failNode;
}

} // namespace puyasol::builder::sol_ast
