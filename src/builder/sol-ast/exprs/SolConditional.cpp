/// @file SolConditional.cpp

#include "builder/sol-ast/exprs/SolConditional.h"
#include "builder/sol-ast/EvmSlotLowering.h"
#include "builder/sol-types/TypeMapper.h"
#include "builder/sol-types/ConversionPlan.h"
// Uses solc AST/Type definitions directly; the hub headers only
// forward-declare them now.
#include <libsolidity/ast/AST.h>
#include <libsolidity/ast/Types.h>

namespace puyasol::builder::sol_ast
{

SolConditional::SolConditional(
	eb::ContractContext& _ctx,
	solidity::frontend::Conditional const& _node)
	: SolExpression(_ctx, _node), m_conditional(_node)
{
}

std::shared_ptr<awst::Expression> SolConditional::toAwst()
{
	auto const* solType = m_conditional.annotation().type;
	auto const* wtype = m_ctx.typeMapper.map(solType);
	auto condition = m_ctx.pinIfWriteBacks(
		m_ctx.lower(m_conditional.condition(), false), m_loc);
	// Assignment conditions must execute even if their enclosing value is discarded.
	if (dynamic_cast<awst::AssignmentExpression*>(condition.get()))
		condition = m_ctx.emitSequencedOperand({}, std::move(condition), true, m_loc);

	auto branch = [&](solidity::frontend::Expression const& expression) {
		return m_ctx.lowerOperand([&] {
			auto value = buildExpr(expression);
			value = EvmSlotLowering::materializeRefValue(m_ctx, m_scope,
				std::move(value), expression.annotation().type, wtype, m_loc);
			auto const* sourceType = expression.annotation().type;
			// TypeChecker::visit(Conditional) combines mobile types. A literal
			// becomes string memory here even for non-UTF8 bytes; this is not
			// the ordinary implicit literal-to-string assignment rule.
			if (dynamic_cast<solidity::frontend::StringLiteralType const*>(sourceType))
				sourceType = sourceType->mobileType();
			return ConversionPlan{sourceType, solType, wtype,
				ConversionPlan::Context::Initialization}.emit(
					std::move(value), m_loc, &m_ctx.preEffects());
		});
	};
	// Build in source order; each frame owns lowering, materialization and conversion.
	auto whenTrue = branch(m_conditional.trueExpression());
	auto whenFalse = branch(m_conditional.falseExpression());
	return m_ctx.emitConditional(std::move(condition), std::move(whenTrue),
		std::move(whenFalse), wtype, m_loc);
}

} // namespace puyasol::builder::sol_ast
