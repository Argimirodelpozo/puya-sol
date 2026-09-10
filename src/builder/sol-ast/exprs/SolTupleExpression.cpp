/// @file SolTupleExpression.cpp — tuple/inline-array expression translation.

#include "builder/sol-ast/exprs/SolTupleExpression.h"
#include "builder/sol-eb/AssignmentHelper.h"
#include "builder/sol-types/TypeMapper.h"
#include "builder/sol-types/ConversionPlan.h"
// Uses solc AST/Type definitions directly; the hub headers only
// forward-declare them now.
#include <libsolidity/ast/AST.h>
#include <libsolidity/ast/Types.h>

namespace puyasol::builder::sol_ast
{

SolTupleExpression::SolTupleExpression(
	eb::ContractContext& _ctx,
	solidity::frontend::TupleExpression const& _node)
	: SolExpression(_ctx, _node), m_tuple(_node)
{
}

std::shared_ptr<awst::Expression> SolTupleExpression::toAwst()
{
	if (m_tuple.isInlineArray())
	{
		auto const* solType = dynamic_cast<solidity::frontend::ArrayType const*>(m_tuple.annotation().type);
		assert(solType);
		auto const* wtype = m_ctx.typeMapper.map(solType);
		auto const* elementType = awst::arrayElementType(wtype);
		auto const* nativeElement = m_ctx.typeMapper.map(solType->baseType());
		auto result = awst::makeNewArray(wtype, m_loc);
		for (auto const& component: m_tuple.components())
		{
			assert(component);
			auto lowered = m_ctx.lowerOperand([&] {
				auto value = buildExpr(*component);
				value = ConversionPlan{component->annotation().type, solType->baseType(),
					nativeElement, ConversionPlan::Context::Initialization}.emit(
						std::move(value), m_loc, &m_ctx.preEffects());
				return eb::AssignmentHelper::arc4EncodeForType(
					m_ctx, std::move(value), elementType, m_loc);
			}, false);
			// Solc evaluates array elements in source order. Finish each value and
			// its write-backs before a later element can change what it reads.
			result->values.push_back(m_ctx.emitSequencedOperand(
				std::move(lowered.effects), std::move(lowered.value), true, m_loc));
		}
		return result;
	}

	// Single-element tuple is parenthesization
	if (m_tuple.components().size() == 1 && m_tuple.components()[0])
		return buildExpr(*m_tuple.components()[0]);

	// Missing LHS components are represented by empty-name placeholders.
	auto e = awst::makeTupleExpression(nullptr, m_loc);
	std::vector<awst::WType const*> types;
	for (auto const& comp: m_tuple.components())
	{
		auto value = comp ? buildExpr(*comp)
			: awst::makeVarExpression("", awst::WType::uint64Type(), m_loc);
		types.push_back(value->wtype);
		e->items.push_back(std::move(value));
	}
	e->wtype = m_ctx.typeMapper.createType<awst::WTuple>(std::move(types), std::nullopt);
	return e;
}

} // namespace puyasol::builder::sol_ast
