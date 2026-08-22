/// @file CalldataMapOps.cpp
/// EVM-calldata offset map + flat-element access for Yul translation.
/// computeFlatElementCount/computeARC4ByteSize: pure type walks.
/// initializeCalldataMap: populates m_calldataMap + m_localConstants per block.
/// accessFlatElement: AWST expression for the i-th 32-byte EVM calldata slot.

#include "builder/assembly/AssemblyBuilder.h"
// yul nodes BY VALUE (the AST aliases are std::variant, which needs
// complete types). Kept out of AssemblyBuilder.h so only the TUs that
// actually instantiate them pay the ~223k lines.
#include <libyul/AST.h>
#include <libyul/Dialect.h>

namespace puyasol::builder
{

int AssemblyBuilder::computeFlatElementCount(awst::WType const* _type)
{
	if (!_type)
		return 1;
	if (_type->kind() == awst::WTypeKind::ReferenceArray)
	{
		auto const* refArr = dynamic_cast<awst::ReferenceArray const*>(_type);
		if (refArr && refArr->arraySize())
			return *refArr->arraySize() * computeFlatElementCount(refArr->elementType());
	}
	if (_type->kind() == awst::WTypeKind::ARC4StaticArray)
	{
		auto const* arc4Arr = dynamic_cast<awst::ARC4StaticArray const*>(_type);
		if (arc4Arr)
			return arc4Arr->arraySize() * computeFlatElementCount(arc4Arr->elementType());
	}
	// ARC4Struct: must sum field counts. Without this, G1Point appeared as 1 slot,
	// so mload(mload(base)) folded the whole struct into ARC4Decode<biguint> — puya rejects that.
	if (_type->kind() == awst::WTypeKind::ARC4Struct)
	{
		auto const* arc4Struct = dynamic_cast<awst::ARC4Struct const*>(_type);
		if (arc4Struct)
		{
			int total = 0;
			for (auto const& [name, fieldType]: arc4Struct->fields())
				total += computeFlatElementCount(fieldType);
			return total;
		}
	}
	return 1;
}

int AssemblyBuilder::computeARC4ByteSize(awst::WType const* _type)
{
	if (!_type)
		return 32;
	if (auto const* uintN = dynamic_cast<awst::ARC4UIntN const*>(_type))
		return uintN->n() / 8;
	if (auto const* arr = dynamic_cast<awst::ARC4StaticArray const*>(_type))
		return arr->arraySize() * computeARC4ByteSize(arr->elementType());
	if (auto const* s = dynamic_cast<awst::ARC4Struct const*>(_type))
	{
		int total = 0;
		for (auto const& [name, fieldType]: s->fields())
			total += computeARC4ByteSize(fieldType);
		return total;
	}
	if (auto const* bytesType = dynamic_cast<awst::BytesWType const*>(_type))
	{
		if (bytesType->length())
			return *bytesType->length();
	}
	return 32; // default
}

void AssemblyBuilder::initializeCalldataMap(
	std::vector<std::pair<std::string, awst::WType const*>> const& _params
)
{
	uint64_t offset = 4; // skip 4-byte selector
	for (auto const& [name, type]: _params)
	{
		uint64_t headBytes = calldataHeadSizeOf(name, type);
		// ONE entry per EVM head WORD (bug the fuzzer found: computeFlatElementCount
		// counts a bytes4 element as 4 leaves, so bytes4[2] made 8 byte-granular
		// entries where the head is 2 words — offsets past the head aliased the
		// next param and word-index retrieval mis-navigated). flatIndex is the
		// WORD index; the retrieval navigates the solc structure (accessEvmLeaf).
		int words = static_cast<int>(headBytes / 32);
		if (words < 1) words = 1;
		m_localConstants[name] = offset;
		m_calldataParamNames.insert(name);
		for (int i = 0; i < words; ++i)
		{
			CalldataElement elem;
			elem.paramName = name;
			elem.flatIndex = i;
			elem.paramType = type;
			m_calldataMap[offset + static_cast<uint64_t>(i) * 32] = elem;
		}
		offset += headBytes;
	}
}

std::shared_ptr<awst::Expression> AssemblyBuilder::accessFlatElement(
	std::shared_ptr<awst::Expression> _base,
	awst::WType const* _type,
	int _flatIndex,
	awst::SourceLocation const& _loc
)
{
	if (_type && _type->kind() == awst::WTypeKind::ReferenceArray)
	{
		auto const* refArr = dynamic_cast<awst::ReferenceArray const*>(_type);
		if (!refArr || !refArr->arraySize())
			return _base;

		int innerSize = computeFlatElementCount(refArr->elementType());
		int outerIndex = _flatIndex / innerSize;
		int innerFlatIndex = _flatIndex % innerSize;

		auto index = awst::makeIntegerConstant(outerIndex, _loc);

		auto indexExpr = awst::makeIndexExpression(_base, std::move(index), refArr->elementType(), _loc);

		if (innerSize == 1)
			return indexExpr;

		return accessFlatElement(indexExpr, refArr->elementType(), innerFlatIndex, _loc);
	}

	if (_type && _type->kind() == awst::WTypeKind::ARC4StaticArray)
	{
		auto const* arc4Arr = dynamic_cast<awst::ARC4StaticArray const*>(_type);
		if (!arc4Arr)
			return _base;

		int innerSize = computeFlatElementCount(arc4Arr->elementType());
		int outerIndex = _flatIndex / innerSize;
		int innerFlatIndex = _flatIndex % innerSize;

		auto index = awst::makeIntegerConstant(outerIndex, _loc);

		auto indexExpr = awst::makeIndexExpression(_base, std::move(index), arc4Arr->elementType(), _loc);

		if (innerSize == 1)
			return awst::makeARC4Decode(indexExpr, awst::WType::biguintType(), _loc);

		return accessFlatElement(indexExpr, arc4Arr->elementType(), innerFlatIndex, _loc);
	}

	// ARC4Struct: pick field at _flatIndex and emit FieldExpression.
	// Without this, mload(mload(struct_ptr_array)) would wrap the whole struct
	// in ARC4Decode<biguint>, which puya rejects.
	if (_type && _type->kind() == awst::WTypeKind::ARC4Struct)
	{
		auto const* arc4Struct = dynamic_cast<awst::ARC4Struct const*>(_type);
		if (!arc4Struct)
			return _base;

		int cursor = 0;
		for (auto const& [name, fieldType] : arc4Struct->fields())
		{
			int fieldSize = computeFlatElementCount(fieldType);
			if (_flatIndex < cursor + fieldSize)
			{
				int innerFlatIndex = _flatIndex - cursor;
				auto fieldExpr = awst::makeFieldExpression(
					_base, name, fieldType, _loc);
				if (fieldSize == 1)
					return awst::makeARC4Decode(std::move(fieldExpr), awst::WType::biguintType(), _loc);
				return accessFlatElement(
					std::move(fieldExpr), fieldType,
					innerFlatIndex, _loc);
			}
			cursor += fieldSize;
		}
		return _base; // _flatIndex past all fields — unreachable; avoid crash
	}

	return _base; // scalar; _flatIndex should be 0
}

} // namespace puyasol::builder
