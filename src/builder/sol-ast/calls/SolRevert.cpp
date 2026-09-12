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
	RevertPayload payload;
	payload.message = "revert";
	if (dynamic_cast<solidity::frontend::ErrorDefinition const*>(
			solidity::frontend::ASTNode::referencedDeclaration(m_call.expression())))
		payload = RevertPayload(m_ctx, m_call, m_loc);
	else if (!arguments().empty())
		payload = RevertPayload(CallOperands::evaluate(m_ctx, *arguments()[0], m_loc), m_loc);
	auto& errorMessage = payload.message;
	auto& revertBlob = payload.blob;
	auto failNode = awst::makeAssert(
		awst::makeFalse(m_loc), m_loc, std::move(errorMessage));
	if (revertBlob)
	{
		m_ctx.preEffects().push_back(
			makeRevertLogStmt(std::move(revertBlob), m_loc));
		// isExplicit=false: let puya optimizer strip provably-unreachable fail.
		failNode->isExplicit = false;
	}
	return failNode;
}

} // namespace puyasol::builder::sol_ast
