#include "builder/sol-ast/CallOperands.h"
#include "builder/SolcFacts.h"
#include "builder/sol-eb/ContractContext.h"
#include <algorithm>
#include <numeric>

namespace puyasol::builder::sol_ast
{

std::vector<size_t> CallOperands::argumentOrder(
	solidity::frontend::FunctionCall const& call, bool viaIR)
{
	auto const sorted = call.sortedArguments();
	std::vector<size_t> order(sorted.size());
	std::iota(order.begin(), order.end(), 0);
	if (viaIR)
	{
		// IR traversal visits source arguments; sortedArguments is only the
		// formal-parameter mapping (AST_accept.h / AST.cpp in pinned solc).
		auto const source = call.arguments();
		for (size_t i = 0; i < source.size(); ++i)
			order[i] = std::find(sorted.begin(), sorted.end(), source[i]) - sorted.begin();
	}
	else if (auto const* type = dynamic_cast<solidity::frontend::FunctionType const*>(
			call.expression().annotation().type))
	{
		using Kind = solidity::frontend::FunctionType::Kind;
		// Legacy ExpressionCompiler evaluates modulus, rhs, lhs.
		if (type->kind() == Kind::AddMod || type->kind() == Kind::MulMod)
			std::reverse(order.begin(), order.end());
	}
	return order;
}

std::vector<CallOperands::Expr> CallOperands::build(eb::ContractContext& ctx,
	solidity::frontend::FunctionCall const& call, awst::SourceLocation const& loc,
	LowerArgument const& lower)
{
	auto const sorted = call.sortedArguments();
	std::vector<Expr> values(sorted.size());
	for (auto i: argumentOrder(call, ctx.viaIRSequencing))
		values[i] = evaluate(ctx, *sorted[i], loc, [&] {
			return lower ? lower(*sorted[i], i) : ctx.buildExpr(*sorted[i]);
		});
	return values;
}

std::vector<CallOperands::Expr> CallOperands::buildParameters(eb::ContractContext& ctx,
	solidity::frontend::FunctionCall const& call, awst::SourceLocation const& loc,
	LowerArgument const& lower)
{
	auto const* type = dynamic_cast<solidity::frontend::FunctionType const*>(call.expression().annotation().type);
	bool const bound = type && type->hasBoundFirstArgument();
	Expr receiver;
	auto bindReceiver = [&] {
		auto const& source = *SolcFacts::callArguments(call).front();
		receiver = evaluate(ctx, source, loc, [&] { return lower ? lower(source, 0) : ctx.buildExpr(source); });
	};
	// Legacy evaluates arguments before the receiver; IR visits the callee first.
	if (bound && ctx.viaIRSequencing) bindReceiver();
	auto values = build(ctx, call, loc,
		[&](auto const& source, size_t i) { return lower ? lower(source, i + bound) : ctx.buildExpr(source); });
	if (bound)
	{
		if (!ctx.viaIRSequencing) bindReceiver();
		values.insert(values.begin(), std::move(receiver));
	}
	return values;
}

CallOperands::Expr CallOperands::evaluate(eb::ContractContext& ctx,
	solidity::frontend::Expression const& source, awst::SourceLocation const& loc,
	std::function<Expr()> const& lower)
{
	auto operand = ctx.lowerOperand([&] { return lower ? lower() : ctx.buildExpr(source); }, false);
	// Mutable lvalues remain referable. Temporaries must execute now even
	// when their carrier is mutable; encoding waits until every argument runs.
	bool pin = operand.value && operand.value->wtype
		&& (operand.value->wtype->immutable() || !*source.annotation().isLValue);
	return ctx.emitSequencedOperand(std::move(operand.effects), std::move(operand.value), pin, loc);
}

} // namespace puyasol::builder::sol_ast
