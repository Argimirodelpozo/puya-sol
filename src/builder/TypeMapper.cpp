#include "builder/TypeMapper.h"
#include "Logger.h"

#include <libsolidity/ast/AST.h>

namespace puyasol::builder
{

awst::WType const* TypeMapper::map(solidity::frontend::Type const* _solType)
{
	using namespace solidity::frontend;

	if (!_solType)
		return awst::WType::voidType();

	// Check cache first
	std::string const typeStr = _solType->toString(true);
	auto it = m_cache.find(typeStr);
	if (it != m_cache.end())
		return it->second;

	awst::WType const* result = nullptr;

	switch (_solType->category())
	{
	case Type::Category::Bool:
		result = awst::WType::boolType();
		break;

	case Type::Category::Integer:
	{
		auto const* intType = dynamic_cast<IntegerType const*>(_solType);
		if (intType)
		{
			unsigned bits = intType->numBits();
			if (bits <= 64)
				result = awst::WType::uint64Type();
			else
				result = awst::WType::biguintType();
		}
		break;
	}

	case Type::Category::Address:
		result = awst::WType::accountType();
		break;

	case Type::Category::FixedBytes:
	{
		auto const* fbType = dynamic_cast<FixedBytesType const*>(_solType);
		if (fbType)
			result = createType<awst::BytesWType>(static_cast<int>(fbType->numBytes()));
		break;
	}

	case Type::Category::StringLiteral:
	case Type::Category::Array:
	{
		auto const* arrType = dynamic_cast<ArrayType const*>(_solType);
		if (arrType)
		{
			if (arrType->isString())
				result = awst::WType::stringType();
			else if (arrType->isByteArrayOrString())
				result = awst::WType::bytesType();
			else
			{
				awst::WType const* elemType = map(arrType->baseType());
				if (!arrType->isDynamicallySized())
				{
					// Static array (e.g. bool[3]) — preserve size
					int len = static_cast<int>(arrType->length());
					// For nested static arrays (e.g. uint[8][28]), use ARC4 encoding
					// for the inner array as element type, since puya's ReferenceArray
					// requires immutable elements and ReferenceArray itself is mutable.
					// ARC4StaticArray IS immutable, so it can be a ReferenceArray element.
					auto const* innerArr = dynamic_cast<awst::ReferenceArray const*>(elemType);
					if (innerArr && innerArr->arraySize())
					{
						// Use ARC4 representation of the inner array as the element type
						auto const* arc4ElemType = mapToARC4Type(innerArr->elementType());
						elemType = createType<awst::ARC4StaticArray>(arc4ElemType, *innerArr->arraySize());
					}
					result = createType<awst::ReferenceArray>(elemType, true, len);
				}
				else
				{
					// Dynamic array
					// Puya only supports reference arrays with fixed-size elements.
					// biguint (uint256) is fixed-size (32 bytes) in AVM, so treat it as fixed.
					bool isFixed = elemType
						&& elemType != awst::WType::stringType()
						&& elemType != awst::WType::bytesType();
					if (isFixed)
						result = createType<awst::ReferenceArray>(elemType, true);
					else
					{
						Logger::instance().warning("array of dynamic-size elements not supported, using bytes");
						result = awst::WType::bytesType();
					}
				}
			}
		}
		else
			result = awst::WType::stringType();
		break;
	}

	case Type::Category::Struct:
	{
		auto const* structType = dynamic_cast<StructType const*>(_solType);
		if (structType)
			result = mapStruct(structType);
		break;
	}

	case Type::Category::Contract:
		// Contract types referenced as addresses in Algorand
		result = awst::WType::accountType();
		break;

	case Type::Category::Enum:
		// Enums map to uint64
		result = awst::WType::uint64Type();
		break;

	case Type::Category::UserDefinedValueType:
	{
		// User-defined value types (e.g. `type Fr is uint256`) map to their underlying type
		auto const* udvt = dynamic_cast<UserDefinedValueType const*>(_solType);
		if (udvt)
			result = map(&udvt->underlyingType());
		break;
	}

	case Type::Category::Mapping:
		// Mappings are handled at storage level, not as a type
		// Return bytes as placeholder; StorageMapper handles the box mapping
		result = awst::WType::bytesType();
		break;

	case Type::Category::RationalNumber:
	{
		auto const* ratType = dynamic_cast<RationalNumberType const*>(_solType);
		if (ratType)
		{
			// Rational numbers become the mobile type (integer) they resolve to
			auto const* mobileType = ratType->mobileType();
			if (mobileType)
				result = map(mobileType);
			else
				result = awst::WType::biguintType();
		}
		else
			result = awst::WType::biguintType();
		break;
	}

	case Type::Category::Tuple:
	{
		auto const* tupleType = dynamic_cast<TupleType const*>(_solType);
		if (tupleType)
		{
			if (tupleType->components().empty())
			{
				// Empty tuple = void (e.g. return type of void function call)
				result = awst::WType::voidType();
			}
			else
			{
				std::vector<awst::WType const*> types;
				for (auto const& comp: tupleType->components())
					types.push_back(map(comp));
				result = createType<awst::WTuple>(std::move(types));
			}
		}
		break;
	}

	default:
		// Unsupported type — return bytes as fallback
		Logger::instance().warning("unsupported type '" + typeStr + "', falling back to bytes");
		result = awst::WType::bytesType();
		break;
	}

	if (result)
		m_cache[typeStr] = result;
	else
		result = awst::WType::voidType();

	return result;
}

awst::WType const* TypeMapper::mapToARC4Type(awst::WType const* _type)
{
	if (!_type)
		return nullptr;

	// Already ARC4 — pass through unchanged
	switch (_type->kind())
	{
	case awst::WTypeKind::ARC4UIntN:
	case awst::WTypeKind::ARC4UFixedNxM:
	case awst::WTypeKind::ARC4Tuple:
	case awst::WTypeKind::ARC4DynamicArray:
	case awst::WTypeKind::ARC4StaticArray:
	case awst::WTypeKind::ARC4Struct:
		return _type;
	default:
		break;
	}

	auto const* arc4Byte = createType<awst::ARC4UIntN>(8);

	if (_type == awst::WType::uint64Type())
		return createType<awst::ARC4UIntN>(64);
	if (_type == awst::WType::biguintType())
		return createType<awst::ARC4UIntN>(512);
	if (_type == awst::WType::boolType())
		return createType<awst::ARC4UIntN>(8);
	if (_type == awst::WType::accountType())
		return createType<awst::ARC4StaticArray>(arc4Byte, 32);
	if (_type == awst::WType::bytesType())
		return createType<awst::ARC4DynamicArray>(arc4Byte);
	if (_type == awst::WType::stringType())
		return createType<awst::ARC4DynamicArray>(arc4Byte);

	if (_type->kind() == awst::WTypeKind::Bytes)
	{
		auto const* bytesType = static_cast<awst::BytesWType const*>(_type);
		if (bytesType->length().has_value())
			return createType<awst::ARC4StaticArray>(arc4Byte, bytesType->length().value());
		return createType<awst::ARC4DynamicArray>(arc4Byte);
	}

	// ReferenceArray → ARC4StaticArray (if sized) or ARC4DynamicArray
	if (_type->kind() == awst::WTypeKind::ReferenceArray)
	{
		auto const* refArr = static_cast<awst::ReferenceArray const*>(_type);
		auto const* arc4Elem = mapToARC4Type(refArr->elementType());
		if (refArr->arraySize().has_value())
			return createType<awst::ARC4StaticArray>(arc4Elem, refArr->arraySize().value());
		return createType<awst::ARC4DynamicArray>(arc4Elem);
	}

	// WTuple → ARC4Tuple
	if (_type->kind() == awst::WTypeKind::WTuple)
	{
		auto const* tupleType = static_cast<awst::WTuple const*>(_type);
		std::vector<awst::WType const*> arc4Types;
		for (auto const* t: tupleType->types())
			arc4Types.push_back(mapToARC4Type(t));
		return createType<awst::ARC4Tuple>(std::move(arc4Types));
	}

	// Fallback: return as-is (best effort)
	return _type;
}

awst::WType const* TypeMapper::mapStruct(solidity::frontend::StructType const* _structType)
{
	if (!_structType)
		return awst::WType::voidType();

	auto const& structDef = _structType->structDefinition();
	std::string name = structDef.name();

	// Check cache
	auto it = m_cache.find("struct:" + name);
	if (it != m_cache.end())
		return it->second;

	// Build ARC4Struct with fields in declaration order
	std::vector<std::pair<std::string, awst::WType const*>> fields;

	for (auto const& member: structDef.members())
	{
		// Skip mapping fields — they are storage-only and cannot be
		// embedded in an ARC4Struct.  Each mapping is handled as a
		// separate box map by StorageMapper / ExpressionTranslator.
		if (member->type()->category() == solidity::frontend::Type::Category::Mapping)
			continue;
		auto const* rawType = map(member->type());
		auto const* arc4Type = mapToARC4Type(rawType);
		fields.emplace_back(member->name(), arc4Type);
	}

	auto* result = createType<awst::ARC4Struct>(
		name,
		std::move(fields),
		/*_frozen=*/false
	);

	m_cache["struct:" + name] = result;
	return result;
}

} // namespace puyasol::builder
