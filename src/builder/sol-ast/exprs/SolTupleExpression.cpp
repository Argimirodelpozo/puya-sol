/// @file SolTupleExpression.cpp
/// Migrated from TupleBuilder.cpp (TupleExpression part).

#include "builder/sol-ast/exprs/SolTupleExpression.h"
#include "builder/sol-types/TypeMapper.h"
#include "builder/sol-types/TypeCoercion.h"

namespace puyasol::builder::sol_ast
{

SolTupleExpression::SolTupleExpression(
	eb::BuilderContext& _ctx,
	solidity::frontend::TupleExpression const& _node)
	: SolExpression(_ctx, _node), m_tuple(_node)
{
}

std::shared_ptr<awst::Expression> SolTupleExpression::toAwst()
{
	// Inline array literals: [val1, val2, ...] → NewArray
	if (m_tuple.isInlineArray())
	{
		auto* wtype = m_ctx.typeMapper.map(m_tuple.annotation().type);
		auto* elementType = awst::WType::uint64Type();
		if (auto const* refArr = dynamic_cast<awst::ReferenceArray const*>(wtype))
			elementType = refArr->elementType();

		auto e = std::make_shared<awst::NewArray>();
		e->sourceLocation = m_loc;
		e->wtype = wtype;
		for (auto const& comp: m_tuple.components())
		{
			if (comp)
			{
				auto val = buildExpr(*comp);
				val = builder::TypeCoercion::implicitNumericCast(
					std::move(val), elementType, m_loc);
				e->values.push_back(std::move(val));
			}
		}
		return e;
	}

	// Single-element tuple is parenthesization
	if (m_tuple.components().size() == 1 && m_tuple.components()[0])
		return buildExpr(*m_tuple.components()[0]);

	// Multi-element tuple
	// Check if this is a LHS tuple with skipped elements (e.g., (,,a) = f())
	bool hasNulls = false;
	for (auto const& comp: m_tuple.components())
		if (!comp) hasNulls = true;

	auto e = std::make_shared<awst::TupleExpression>();
	e->sourceLocation = m_loc;
	std::vector<awst::WType const*> types;

	if (hasNulls)
	{
		// LHS tuple with gaps: preserve original positions using null-like placeholders.
		// Store the original index in a tag on each item so handleTupleAssignment
		// can map correctly. We use TupleExpression.items with nulls represented
		// as VarExpression with empty name (marker for "skip this position").
		for (size_t i = 0; i < m_tuple.components().size(); ++i)
		{
			auto const& comp = m_tuple.components()[i];
			if (comp)
			{
				auto translated = buildExpr(*comp);
				types.push_back(translated->wtype);
				e->items.push_back(std::move(translated));
			}
			else
			{
				// Null placeholder — mark with empty-name VarExpression
				auto placeholder = std::make_shared<awst::VarExpression>();
				placeholder->sourceLocation = m_loc;
				placeholder->wtype = awst::WType::uint64Type(); // dummy type
				placeholder->name = ""; // empty = skip marker
				types.push_back(awst::WType::uint64Type());
				e->items.push_back(std::move(placeholder));
			}
		}
	}
	else
	{
		for (auto const& comp: m_tuple.components())
		{
			if (comp)
			{
				auto translated = buildExpr(*comp);
				types.push_back(translated->wtype);
				e->items.push_back(std::move(translated));
			}
		}
	}
	e->wtype = m_ctx.typeMapper.createType<awst::WTuple>(std::move(types), std::nullopt);
	return e;
}

} // namespace puyasol::builder::sol_ast
