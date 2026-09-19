/// @file SolArrayBuilder.cpp
/// Solidity typed array builder — keeps element places separate from read validation.

#include "builder/eb/SolArrayBuilder.h"
#include "builder/codec/Arc4Defaults.h"
#include "builder/types/TypeCoercion.h"
#include "builder/types/TypeMapper.h"
#include "builder/storage/StorageMapper.h"

#include <libsolidity/ast/AST.h>

namespace puyasol::builder::eb
{

std::unique_ptr<InstanceBuilder> SolArrayBuilder::index(
	InstanceBuilder& _idx, awst::SourceLocation const& _loc)
{
	auto base = resolve();
	auto index = _idx.resolve();

	auto* elemType = awst::arrayElementType(wtype());
	if (!elemType)
		return nullptr;
	// Logical bounds are effects of the source access, not of consuming its
	// result. Physical extraction alone is insufficient for nested or discarded reads.
	auto length = m_arrayType->isDynamicallySized()
		? awst::makeArrayLength(base, awst::WType::uint64Type(), _loc)
		: std::shared_ptr<awst::Expression>(awst::makeIntegerConstant(m_arrayType->length().str(), _loc));
	index = TypeCoercion::checkedIndexToUint64(m_ctx.preEffects(), std::move(index), _loc, std::move(length));

	auto e = awst::makeIndexExpression(std::move(base), std::move(index), elemType, _loc);

	auto* expectedType = m_ctx.typeMapper.map(m_arrayType->baseType());
	// Aggregate elements already have a usable ARC4 representation, including
	// finite recursive projections; decoding to the full type would lose the place.
	bool needsDecode = !awst::structurallyEquivalent(elemType, expectedType)
		&& builder::isArc4EncodedType(elemType) && !builder::isArc4EncodedType(expectedType);

	std::shared_ptr<awst::Expression> result = std::move(e);
	if (needsDecode)
		result = awst::makeARC4Decode(std::move(result), expectedType, _loc);

	auto out = std::make_unique<SolArrayBuilder>(m_ctx, m_arrayType, std::move(result));
	out->m_elementType = m_arrayType->baseType();
	out->m_elementLoc = _loc;
	return out;
}

std::shared_ptr<awst::Expression> SolArrayBuilder::resolve()
{
	// Validation and signed cleanup belong to reads, never to writable places.
	if (m_elementType)
	{
		auto value = m_expr;
		if (m_elementType->isValueType())
			value = StorageMapper::makePartialBoxReadWithDefault(
				m_ctx.typeMapper, std::move(value), m_ctx.preEffects(), m_elementLoc);
		value = TypeCoercion::checkedEnum(std::move(value), m_elementType, m_elementLoc, &m_ctx.preEffects());
		return TypeCoercion::signExtendSignedElement(std::move(value), m_elementType, m_elementLoc);
	}
	return m_expr;
}

std::shared_ptr<awst::Expression> SolArrayBuilder::resolve_lvalue()
{
	// Assignment target: the bare decoded element. Never sign-extend (a
	// CommaExpression is not a valid lvalue).
	return m_expr;
}

} // namespace puyasol::builder::eb
