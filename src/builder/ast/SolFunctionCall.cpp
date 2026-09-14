#include "builder/ast/SolFunctionCall.h"
#include "builder/types/TypeCoercion.h"
#include "builder/solc/SolcFacts.h"

namespace puyasol::builder::sol_ast
{

SolFunctionCall::SolFunctionCall(
	eb::ContractContext& _ctx,
	solidity::frontend::FunctionCall const& _call)
	: SolExpression(_ctx, _call),
	  m_call(_call),
	  m_arguments(_call.arguments())
{
}

solidity::frontend::Expression const& SolFunctionCall::funcExpression() const
{
	return SolcFacts::functionExpression(m_call.expression());
}

std::shared_ptr<awst::Expression> SolFunctionCall::extractCallValue()
{
	auto const* opts = dynamic_cast<solidity::frontend::FunctionCallOptions const*>(
		&m_call.expression());
	if (!opts) return nullptr;

	auto const& optNames = opts->names();
	auto optValues = opts->options();
	std::shared_ptr<awst::Expression> value;
	for (size_t i = 0; i < optNames.size(); ++i)
	{
		if (*optNames[i] == "value" && i < optValues.size())
		{
			auto val = CallOperands::evaluate(m_ctx, *optValues[i], m_loc);
			// {value: X}: assert X fits in uint64 before truncating (a >2^64
			// value would silently send `X mod 2^64` microAlgos).
			value = TypeCoercion::checkedAmountToUint64(
				m_ctx.preEffects(), std::move(val), m_loc);
		}
		else if (*optNames[i] == "gas" && i < optValues.size())
		{
			// The gas AMOUNT has no AVM analogue (opcode budget is pooled),
			// but solc EVALUATES option expressions — dropping the expression
			// unevaluated would lose `{gas: f()}` side effects. Evaluate and
			// discard; effect-free shapes (the common `{gas: 200}` literal,
			// a bare local) stay unemitted.
			m_ctx.evaluateForEffects(*optValues[i], m_loc);
		}
	}
	return value;
}

} // namespace puyasol::builder::sol_ast
