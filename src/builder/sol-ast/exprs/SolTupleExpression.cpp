/// @file SolTupleExpression.cpp
/// Migrated from TupleBuilder.cpp (TupleExpression part).

#include "builder/sol-ast/exprs/SolTupleExpression.h"
#include "builder/sol-types/TypeMapper.h"
#include "builder/sol-types/TypeCoercion.h"

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
	// Inline array literals: [val1, val2, ...] → NewArray
	if (m_tuple.isInlineArray())
	{
		auto* wtype = m_ctx.typeMapper.map(m_tuple.annotation().type);
		awst::WType const* elementType = awst::WType::uint64Type();
		if (auto const* refArr = dynamic_cast<awst::ReferenceArray const*>(wtype))
			elementType = refArr->elementType();
		else if (auto const* arc4Static = dynamic_cast<awst::ARC4StaticArray const*>(wtype))
			elementType = arc4Static->elementType();
		else if (auto const* arc4Dyn = dynamic_cast<awst::ARC4DynamicArray const*>(wtype))
			elementType = arc4Dyn->elementType();

		auto e = awst::makeNewArray(wtype, m_loc);
		for (auto const& comp: m_tuple.components())
		{
			if (comp)
			{
				auto val = buildExpr(*comp);
				// For ARC4 element types, cast to native equivalent first, then encode
				if (val->wtype != elementType)
				{
					// Determine the native type that this ARC4 element expects
					awst::WType const* nativeTarget = elementType;
					if (auto const* arc4uint = dynamic_cast<awst::ARC4UIntN const*>(elementType))
						nativeTarget = arc4uint->n() <= 64
							? awst::WType::uint64Type()
							: awst::WType::biguintType();
					else if (elementType == awst::WType::arc4BoolType())
						nativeTarget = awst::WType::boolType();

					// Cast to native type first (e.g. biguint → uint64)
					if (val->wtype != nativeTarget)
						val = builder::TypeCoercion::implicitNumericCast(
							std::move(val), nativeTarget, m_loc);

					// ARC4Encode if still not matching element type (native → ARC4)
					if (val->wtype != elementType)
					{
						// Bytes-backed value targeting an ARC4 aggregate (fn-ptr as
						// ARC4StaticArray<uint8,N>): reinterpret via ARC4FromBytes
						// rather than ARC4Encode which would expect a tuple.
						if (auto const* bw = dynamic_cast<awst::BytesWType const*>(val->wtype))
						{
							bool isArc4Aggregate =
								dynamic_cast<awst::ARC4StaticArray const*>(elementType)
								|| dynamic_cast<awst::ARC4DynamicArray const*>(elementType)
								|| dynamic_cast<awst::ARC4Struct const*>(elementType);
							if (isArc4Aggregate)
							{
								e->values.push_back(awst::makeARC4FromBytes(std::move(val), elementType, m_loc));
								continue;
							}
						}
						// For sub-64-bit ARC4 types, encode via the full-width
						// ARC4 type first, then reinterpret to target width.
						auto const* arc4uint = dynamic_cast<awst::ARC4UIntN const*>(elementType);
						if (arc4uint && arc4uint->n() < 64 && val->wtype == awst::WType::uint64Type())
						{
							// Encode uint64 → arc4.uint64, then ReinterpretCast to arc4.uintN
							auto fullEncode = awst::makeARC4Encode(std::move(val), m_ctx.typeMapper.createType<awst::ARC4UIntN>(64), m_loc);

							// Extract last N/8 bytes
							auto startConst = awst::makeIntegerConstant(8 - arc4uint->n() / 8, m_loc);
							auto lenConst = awst::makeIntegerConstant(arc4uint->n() / 8, m_loc);

							auto castBytes = awst::makeAsBytes(std::move(fullEncode), m_loc);
							auto extract = awst::makeExtract3(
								std::move(castBytes), std::move(startConst), std::move(lenConst), m_loc);

							auto cast = awst::makeReinterpretCast(std::move(extract), elementType, m_loc);
							val = std::move(cast);
						}
						else
						{
							auto encode = awst::makeARC4Encode(std::move(val), elementType, m_loc);
							val = std::move(encode);
						}
					}
				}
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

	auto e = awst::makeTupleExpression(nullptr, m_loc);
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
				types.push_back(awst::WType::uint64Type());
				e->items.push_back(awst::makeVarExpression(
					"", awst::WType::uint64Type(), m_loc));
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
