/// @file SolArrayBuilder.cpp
/// Solidity typed array builder — keeps element places separate from read validation.

#include "builder/eb/SolArrayBuilder.h"
#include "awst/NameGen.h"
#include "builder/codec/Arc4Defaults.h"
#include "builder/types/TypeCoercion.h"
#include "builder/types/TypeMapper.h"
#include "builder/storage/StorageMapper.h"

#include <libsolidity/ast/AST.h>

namespace puyasol::builder::eb
{

awst::WType const* SolArrayBuilder::elementType() const
{
	auto* baseWType = wtype();
	if (!baseWType)
		return nullptr;

	switch (baseWType->kind())
	{
	case awst::WTypeKind::ARC4DynamicArray:
		return static_cast<awst::ARC4DynamicArray const*>(baseWType)->elementType();
	case awst::WTypeKind::ARC4StaticArray:
		return static_cast<awst::ARC4StaticArray const*>(baseWType)->elementType();
	default:
		return nullptr;
	}
}

std::unique_ptr<InstanceBuilder> SolArrayBuilder::index(
	InstanceBuilder& _idx, awst::SourceLocation const& _loc)
{
	auto base = resolve();
	auto index = _idx.resolve();

	if (index->wtype == awst::WType::biguintType())
		index = TypeCoercion::checkedIndexToUint64(m_ctx.preEffects(), std::move(index), _loc);

	auto* elemType = elementType();
	if (!elemType)
		return nullptr;
	if (m_arrayType && !m_arrayType->isDynamicallySized()
		&& m_arrayType->dataStoredIn(solidity::frontend::DataLocation::Storage))
	{
		// Physical extraction is not a bounds check for nested arrays: an
		// invalid inner index can still point inside the enclosing box.
		auto pinned = awst::makeVarExpression("__fixed_storage_idx_" + std::to_string(
			awst::NameGen::next("SolArrayBuilder.fixedStorageIndex")), index->wtype, _loc);
		m_ctx.preEffects().push_back(awst::makeAssignmentStatement(pinned, std::move(index), _loc));
		index = pinned;
		m_ctx.preEffects().push_back(awst::makeExpressionStatement(awst::makeAssert(
			awst::makeNumericCompare(index, awst::NumericComparison::Lt,
				awst::makeIntegerConstant(m_arrayType->length().str(), _loc), _loc),
			_loc, "array index out of bounds"), _loc));
	}

	// CALLDATA arrays kept as ARC4 VALUES (asm-mode functions skip the native
	// decode, so `s.m[i]` indexes the raw encoding): puya's IndexExpression
	// lowering has NO length check — it relies on the physical extract failing,
	// and an EMPTY array inside a larger encoding (`s.m[0]` with s = ([]))
	// reads adjacent struct bytes instead of reverting (EVM Panic 0x32).
	// Assert idx < the uint16 length prefix. Calldata is never an lvalue, so
	// the eval-once base wrap is safe. Found by the night-3 stmt-del mutant on
	// viaYul/dirty_calldata_struct (the deletion was incidental — the empty
	// inner array was the trigger).
	if (wtype() && wtype()->kind() == awst::WTypeKind::ARC4DynamicArray
		&& m_arrayType
		&& m_arrayType->dataStoredIn(solidity::frontend::DataLocation::CallData))
	{
		base = awst::makeEvalOnce(std::move(base), _loc);
		std::string tmpName = "__sol_cdix_" + std::to_string(
			awst::NameGen::next("SolArrayBuilder.cdIndex"));
		auto tmpVar = [&]() {
			return awst::makeVarExpression(tmpName, awst::WType::uint64Type(), _loc);
		};
		m_ctx.preEffects().push_back(
			awst::makeAssignmentStatement(tmpVar(), std::move(index), _loc));
		auto len = awst::makeExtractUInt16(
			awst::makeReinterpretCast(base, awst::WType::bytesType(), _loc),
			awst::makeZero(_loc), _loc);
		auto cmp = awst::makeNumericCompare(
			tmpVar(), awst::NumericComparison::Lt, std::move(len), _loc);
		m_ctx.preEffects().push_back(awst::makeExpressionStatement(
			awst::makeAssert(std::move(cmp), _loc, "array index out of bounds"), _loc));
		index = tmpVar();
	}

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
