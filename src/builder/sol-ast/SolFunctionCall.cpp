#include "builder/sol-ast/SolFunctionCall.h"
#include "builder/sol-types/TypeCoercion.h"

namespace puyasol::builder::sol_ast
{

SolFunctionCall::SolFunctionCall(
	eb::BuilderContext& _ctx,
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
	// Unwrap parenthesized expressions
	while (auto const* tuple = dynamic_cast<solidity::frontend::TupleExpression const*>(expr))
	{
		auto const& comps = tuple->components();
		if (comps.size() == 1 && comps[0])
			expr = comps[0].get();
		else
			break;
	}
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
			return TypeCoercion::implicitNumericCast(
				std::move(val), awst::WType::uint64Type(), m_loc);
		}
	}
	return nullptr;
}

} // namespace puyasol::builder::sol_ast
