/// @file SolConditional.cpp
/// Migrated from ConditionalBuilder.cpp.

#include "builder/sol-ast/exprs/SolConditional.h"
#include "builder/sol-types/TypeMapper.h"
#include "builder/sol-types/TypeCoercion.h"

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
	auto e = std::make_shared<awst::ConditionalExpression>();
	e->sourceLocation = m_loc;

	e->condition = buildExpr(m_conditional.condition());
	if (dynamic_cast<awst::AssignmentExpression*>(e->condition.get()))
	{
		// Side-effecting condition `(x=f()) ? a : b`: emit as pre-statement so
		// the side effect lands even if the ternary is discarded (e.g. `.selector`).
		// makeEvalOnce prevents running twice — without it `(x=f())?10:20` gave
		// 20/cnt=2 (verified).
		e->condition = awst::makeEvalOnce(e->condition, m_loc);
		auto stmt = awst::makeExpressionStatement(e->condition, m_loc);
		m_ctx.prePendingStatements.push_back(std::move(stmt));
	}

	// Each branch only executes conditionally, so its pre-statements (a `**`
	// square-and-multiply loop, `new C()` inner txns, `arr.push()=x`, checked-op
	// asserts) must be gated behind the condition, not flushed unconditionally.
	// buildScopedOperand captures them out of the flat list (OperandPlan,
	// fable-review item 7); gated into if/else blocks below.
	std::vector<std::shared_ptr<awst::Statement>> trueSideEffects;
	e->trueExpr = m_ctx.buildScopedOperand(
		[&] { return buildExpr(m_conditional.trueExpression()); }, trueSideEffects);

	std::vector<std::shared_ptr<awst::Statement>> falseSideEffects;
	e->falseExpr = m_ctx.buildScopedOperand(
		[&] { return buildExpr(m_conditional.falseExpression()); }, falseSideEffects);

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

	// Either branch had side effects: materialise the result in a temp var
	// and gate the side effects behind the branch condition.
	if (!trueSideEffects.empty() || !falseSideEffects.empty())
	{
		static int s_counter = 0;
		std::string tempName = "__cond_" + std::to_string(s_counter++);
		auto resultType = e->wtype ? e->wtype : awst::WType::biguintType();
		auto tempVar = [&] { return awst::makeVarExpression(tempName, resultType, m_loc); };

		auto trueBlock = eb::ContractContext::makeScopedResultBlock(
			std::move(trueSideEffects), tempVar(), e->trueExpr, m_loc);
		auto falseBlock = eb::ContractContext::makeScopedResultBlock(
			std::move(falseSideEffects), tempVar(), e->falseExpr, m_loc);

		m_ctx.prePendingStatements.push_back(awst::makeIfElse(
			e->condition, std::move(trueBlock), std::move(falseBlock), m_loc));

		return tempVar();
	}

	return e;
}

} // namespace puyasol::builder::sol_ast
