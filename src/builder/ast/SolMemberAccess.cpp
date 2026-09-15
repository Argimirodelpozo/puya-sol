#include "builder/ast/SolMemberAccess.h"

namespace puyasol::builder::sol_ast
{

SolMemberAccess::SolMemberAccess(
	eb::ContractContext& _ctx,
	solidity::frontend::MemberAccess const& _node)
	: SolExpression(_ctx, _node),
	  m_memberAccess(_node)
{
}

std::shared_ptr<awst::Expression> SolMemberAccess::projectFunctionValue(
	eb::ContractContext& ctx, solidity::frontend::Expression const& source,
	awst::WType const* resultType, awst::SourceLocation const& loc,
	std::function<std::shared_ptr<awst::Expression>(solidity::frontend::Expression const&)> const& project)
{
	using namespace solidity::frontend;
	if (auto const* tuple = dynamic_cast<TupleExpression const*>(&source);
		tuple && tuple->components().size() == 1 && tuple->components()[0])
		return projectFunctionValue(ctx, *tuple->components()[0], resultType, loc, project);
	if (auto const* options = dynamic_cast<FunctionCallOptions const*>(&source))
	{
		auto value = ctx.emitSequencedOperand({}, projectFunctionValue(
			ctx, options->expression(), resultType, loc, project), true, loc);
		for (auto const& option: options->options())
			ctx.evaluateForEffects(*option, ctx.makeLoc(option->location()));
		return value;
	}
	if (auto const* conditional = dynamic_cast<Conditional const*>(&source))
	{
		auto condition = ctx.lower(conditional->condition(), false);
		auto value = ctx.emitSequencedOperand(
			std::move(condition.effects), std::move(condition.value), true, loc);
		auto whenTrue = ctx.lowerOperand([&] { return projectFunctionValue(
			ctx, conditional->trueExpression(), resultType, loc, project); });
		auto whenFalse = ctx.lowerOperand([&] { return projectFunctionValue(
			ctx, conditional->falseExpression(), resultType, loc, project); });
		return ctx.emitConditional(std::move(value), std::move(whenTrue),
			std::move(whenFalse), resultType, loc);
	}
	return project(source);
}

} // namespace puyasol::builder::sol_ast
