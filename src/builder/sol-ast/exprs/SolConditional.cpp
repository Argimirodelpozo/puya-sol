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

	// Build condition. If it has side effects (AssignmentExpression),
	// emit the side effect as a statement so it executes even if the
	// enclosing expression is discarded (e.g., (flag=true ? a : b).selector).
	e->condition = buildExpr(m_conditional.condition());
	if (auto* assignExpr = dynamic_cast<awst::AssignmentExpression*>(e->condition.get()))
	{
		// Emit: flag = true; (as statement)
		// shared_ptr copy on e->condition so both stmt and condition reference it
		auto stmt = awst::makeExpressionStatement(e->condition, m_loc);
		m_ctx.prePendingStatements.push_back(std::move(stmt));
		// The ConditionalExpression condition still holds the AssignmentExpression,
		// which evaluates to the assigned value (the condition result).
	}

	// Snapshot prePendingStatements size before each branch so we can
	// detect any branch-local side-effect statements (e.g. the
	// square-and-multiply loop emitted by `**`, `new C()` inner txns,
	// `arr.push() = x` rewrites). Side-effect statements from a branch
	// must NOT escape into the outer scope unconditionally — they have to
	// run only when their branch fires. We move them into `if (cond) {…}
	// else {…}` blocks below.
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

	// If either branch had side-effect statements, materialise the result
	// into a temp var and gate the side-effect statements behind the
	// branch's condition. The expression we hand back is then a read of
	// the temp var, so the surrounding scope sees no leaked statements.
	if (!trueSideEffects.empty() || !falseSideEffects.empty())
	{
		static int s_counter = 0;
		std::string tempName = "__cond_" + std::to_string(s_counter++);
		auto resultType = e->wtype ? e->wtype : awst::WType::biguintType();

		auto trueBlock = std::make_shared<awst::Block>();
		trueBlock->sourceLocation = m_loc;
		for (auto& s: trueSideEffects)
			trueBlock->body.push_back(std::move(s));
		{
			auto target = awst::makeVarExpression(tempName, resultType, m_loc);
			trueBlock->body.push_back(
				awst::makeAssignmentStatement(target, e->trueExpr, m_loc));
		}

		auto falseBlock = std::make_shared<awst::Block>();
		falseBlock->sourceLocation = m_loc;
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
