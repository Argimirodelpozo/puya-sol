#include "builder/sol-ast/SolFunctionCall.h"
#include "builder/sol-types/TypeCoercion.h"

#include <libsolidity/ast/ASTUtils.h>

namespace puyasol::builder::sol_ast
{

SolFunctionCall::SolFunctionCall(
	eb::ContractContext& _ctx,
	solidity::frontend::FunctionCall const& _call)
	: SolExpression(_ctx, _call),
	  m_call(_call)
{
}

solidity::frontend::Expression const& SolFunctionCall::funcExpression() const
{
	auto const* expr = &m_call.expression();
	// Unwrap FunctionCallOptions
	if (auto const* opts = dynamic_cast<solidity::frontend::FunctionCallOptions const*>(expr))
		expr = &opts->expression();
	// Unwrap parenthesized expressions (1-element TupleExpressions).
	expr = solidity::frontend::resolveOuterUnaryTuples(expr);
	return *expr;
}

std::shared_ptr<awst::Expression> SolFunctionCall::extractCallValue()
{
	auto const* opts = dynamic_cast<solidity::frontend::FunctionCallOptions const*>(
		&m_call.expression());
	if (!opts) return nullptr;

	auto const& optNames = opts->names();
	auto optValues = opts->options();
	for (size_t i = 0; i < optNames.size(); ++i)
	{
		if (*optNames[i] == "value" && i < optValues.size())
		{
			auto val = buildExpr(*optValues[i]);
			// {value: X}: assert X fits in uint64 before truncating (a >2^64
			// value would silently send `X mod 2^64` microAlgos).
			return TypeCoercion::checkedAmountToUint64(
				m_ctx.prePendingStatements, std::move(val), m_loc);
		}
	}
	return nullptr;
}

} // namespace puyasol::builder::sol_ast
