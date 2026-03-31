/// @file SolConditional.cpp
/// Migrated from ConditionalBuilder.cpp.

#include "builder/sol-ast/exprs/SolConditional.h"
#include "builder/sol-types/TypeMapper.h"
#include "builder/sol-types/TypeCoercion.h"

namespace puyasol::builder::sol_ast
{

SolConditional::SolConditional(
	eb::BuilderContext& _ctx,
	solidity::frontend::Conditional const& _node)
	: SolExpression(_ctx, _node), m_conditional(_node)
{
}

std::shared_ptr<awst::Expression> SolConditional::toAwst()
{
	auto e = std::make_shared<awst::ConditionalExpression>();
	e->sourceLocation = m_loc;
	e->condition = buildExpr(m_conditional.condition());
	e->trueExpr = buildExpr(m_conditional.trueExpression());
	e->falseExpr = buildExpr(m_conditional.falseExpression());
	e->wtype = m_ctx.typeMapper.map(m_conditional.annotation().type);
	e->trueExpr = builder::TypeCoercion::implicitNumericCast(
		std::move(e->trueExpr), e->wtype, m_loc);
	e->falseExpr = builder::TypeCoercion::implicitNumericCast(
		std::move(e->falseExpr), e->wtype, m_loc);
	return e;
}

} // namespace puyasol::builder::sol_ast
