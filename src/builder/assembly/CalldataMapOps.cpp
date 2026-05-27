/// @file CalldataMapOps.cpp
/// EVM-calldata offset mapping + recursive flat-element access for
/// inline-assembly Yul translation. Extracted from AssemblyBuilder.cpp;
/// these four helpers form a self-contained type/index-math cluster
/// (sister TUs DataOps and MemoryOps call into them but never touch
/// the per-block state they operate on).
///
/// `computeFlatElementCount` + `computeARC4ByteSize` are pure type
/// walks. `initializeCalldataMap` populates `m_calldataMap` +
/// `m_localConstants` at the start of every Yul block.
/// `accessFlatElement` recursively traverses a parameter's structural
/// shape to materialise an AWST expression for the i-th 32-byte slot
/// of EVM calldata.

#include "builder/assembly/AssemblyBuilder.h"

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
	// ARC4Struct (e.g. Honk.G1Point { x, y }): sum of field counts. Without
	// this, computeFlatElementCount(G1Point) was 1, so a ReferenceArray<G1Point>
	// pretended to have 1 flat slot per element — and `mload(mload(base))` patterns
	// folded the whole struct into a single ARC4Decode<biguint>(struct), which
	// puya rejects. Counting fields lets accessFlatElement's struct branch
	// (above) extract the right field at the right flat index.
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
	uint64_t offset = 4; // Skip 4-byte function selector
	for (auto const& [name, type]: _params)
	{
		int elementCount = computeFlatElementCount(type);

		// Store the EVM calldata base offset for this parameter
		m_localConstants[name] = offset;

		// Map each 32-byte element to its parameter and flat index
		for (int i = 0; i < elementCount; ++i)
		{
			CalldataElement elem;
			elem.paramName = name;
			elem.flatIndex = i;
			elem.paramType = type;
			m_calldataMap[offset + static_cast<uint64_t>(i) * 32] = elem;
		}

		offset += static_cast<uint64_t>(elementCount) * 32;
	}
}

std::shared_ptr<awst::Expression> AssemblyBuilder::accessFlatElement(
	std::shared_ptr<awst::Expression> _base,
	awst::WType const* _type,
	int _flatIndex,
	awst::SourceLocation const& _loc
)
{
	// Handle ReferenceArray
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

	// Handle ARC4StaticArray
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
		{
			// For leaf ARC4 elements (like arc4.uint256), decode to native biguint
			auto decode = awst::makeARC4Decode(indexExpr, awst::WType::biguintType(), _loc);
			return decode;
		}

		return accessFlatElement(indexExpr, arc4Arr->elementType(), innerFlatIndex, _loc);
	}

	// Handle ARC4Struct (e.g. Honk's G1Point { x: uint256, y: uint256 }).
	// Inline assembly that does `mload(mload(struct_ptr_array))` ends up
	// here once the calldata-map walk recurses into a struct element. We
	// pick the field at `_flatIndex` (counting nested fields by size) and
	// emit a FieldExpression. Without this branch the call would fall
	// through to the scalar return and the caller would wrap the raw
	// struct in ARC4Decode<biguint>, which puya rejects.
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
				{
					// Leaf field — decode to biguint to match the
					// shape every other accessFlatElement leaf returns
					// (mload's caller expects a biguint scalar).
					return awst::makeARC4Decode(
						std::move(fieldExpr),
						awst::WType::biguintType(), _loc);
				}
				return accessFlatElement(
					std::move(fieldExpr), fieldType,
					innerFlatIndex, _loc);
			}
			cursor += fieldSize;
		}
		// _flatIndex past all fields — should be unreachable; bail to
		// scalar so we at least don't crash the compile.
		return _base;
	}

	// Scalar — _flatIndex should be 0
	return _base;
}

} // namespace puyasol::builder
