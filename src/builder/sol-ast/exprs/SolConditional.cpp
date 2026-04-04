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

	// Build condition. If it has side effects (AssignmentExpression),
	// emit the side effect as a statement so it executes even if the
	// enclosing expression is discarded (e.g., (flag=true ? a : b).selector).
	e->condition = buildExpr(m_conditional.condition());
	if (auto* assignExpr = dynamic_cast<awst::AssignmentExpression*>(e->condition.get()))
	{
		// Emit: flag = true; (as statement)
		auto stmt = std::make_shared<awst::ExpressionStatement>();
		stmt->sourceLocation = m_loc;
		stmt->expr = e->condition; // shared_ptr copy — both stmt and condition reference it
		m_ctx.prePendingStatements.push_back(std::move(stmt));
		// The ConditionalExpression condition still holds the AssignmentExpression,
		// which evaluates to the assigned value (the condition result).
	}

	e->trueExpr = buildExpr(m_conditional.trueExpression());
	e->falseExpr = buildExpr(m_conditional.falseExpression());
	e->wtype = m_ctx.typeMapper.map(m_conditional.annotation().type);

	// Coerce branches to target type. For tuples, coerce element-by-element.
	auto coerceBranch = [&](std::shared_ptr<awst::Expression> branch)
		-> std::shared_ptr<awst::Expression>
	{
		if (e->wtype && e->wtype->kind() == awst::WTypeKind::WTuple)
		{
			auto const* targetTuple = dynamic_cast<awst::WTuple const*>(e->wtype);
			auto* tupleLit = dynamic_cast<awst::TupleExpression*>(branch.get());
			if (targetTuple && tupleLit && tupleLit->items.size() == targetTuple->types().size())
			{
				for (size_t i = 0; i < tupleLit->items.size(); ++i)
					tupleLit->items[i] = builder::TypeCoercion::implicitNumericCast(
						std::move(tupleLit->items[i]), targetTuple->types()[i], m_loc);
				tupleLit->wtype = e->wtype;
			}
			return branch;
		}
		return builder::TypeCoercion::implicitNumericCast(std::move(branch), e->wtype, m_loc);
	};

	e->trueExpr = coerceBranch(std::move(e->trueExpr));
	e->falseExpr = coerceBranch(std::move(e->falseExpr));
	return e;
}

} // namespace puyasol::builder::sol_ast
