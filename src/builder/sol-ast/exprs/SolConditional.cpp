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

	// Snapshot prePending size before each branch to detect branch-local side
	// effects (e.g. `**` square-and-multiply loop, `new C()` inner txns,
	// `arr.push() = x`). Branch side effects must not escape unconditionally;
	// we gate them in if/else blocks below.
	auto preBeforeTrue = m_ctx.prePendingStatements.size();
	e->trueExpr = buildExpr(m_conditional.trueExpression());
	std::vector<std::shared_ptr<awst::Statement>> trueSideEffects;
	if (m_ctx.prePendingStatements.size() > preBeforeTrue)
	{
		trueSideEffects.assign(
			std::make_move_iterator(m_ctx.prePendingStatements.begin() + preBeforeTrue),
			std::make_move_iterator(m_ctx.prePendingStatements.end())
		);
		m_ctx.prePendingStatements.erase(
			m_ctx.prePendingStatements.begin() + preBeforeTrue,
			m_ctx.prePendingStatements.end()
		);
	}

	auto preBeforeFalse = m_ctx.prePendingStatements.size();
	e->falseExpr = buildExpr(m_conditional.falseExpression());
	std::vector<std::shared_ptr<awst::Statement>> falseSideEffects;
	if (m_ctx.prePendingStatements.size() > preBeforeFalse)
	{
		falseSideEffects.assign(
			std::make_move_iterator(m_ctx.prePendingStatements.begin() + preBeforeFalse),
			std::make_move_iterator(m_ctx.prePendingStatements.end())
		);
		m_ctx.prePendingStatements.erase(
			m_ctx.prePendingStatements.begin() + preBeforeFalse,
			m_ctx.prePendingStatements.end()
		);
	}

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

		auto trueBlock = awst::makeBlock(m_loc);
		for (auto& s: trueSideEffects)
			trueBlock->body.push_back(std::move(s));
		{
			auto target = awst::makeVarExpression(tempName, resultType, m_loc);
			trueBlock->body.push_back(
				awst::makeAssignmentStatement(target, e->trueExpr, m_loc));
		}

		auto falseBlock = awst::makeBlock(m_loc);
		for (auto& s: falseSideEffects)
			falseBlock->body.push_back(std::move(s));
		{
			auto target = awst::makeVarExpression(tempName, resultType, m_loc);
			falseBlock->body.push_back(
				awst::makeAssignmentStatement(target, e->falseExpr, m_loc));
		}

		m_ctx.prePendingStatements.push_back(awst::makeIfElse(
			e->condition, std::move(trueBlock), std::move(falseBlock), m_loc));

		return awst::makeVarExpression(tempName, resultType, m_loc);
	}

	return e;
}

} // namespace puyasol::builder::sol_ast
