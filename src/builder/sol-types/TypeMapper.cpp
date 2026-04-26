#include "builder/sol-types/TypeMapper.h"
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
				// Map element type to ARC4, preserving exact bit widths for ABI.
				// Use mapSolTypeToARC4 to avoid uint8→uint64→arc4.uint64 promotion.
				awst::WType const* arc4ElemType = mapSolTypeToARC4(arrType->baseType());
				if (!arrType->isDynamicallySized())
				{
					int64_t len = static_cast<int64_t>(arrType->length());
					result = createType<awst::ARC4StaticArray>(arc4ElemType, len);
				}
				else
				{
					result = createType<awst::ARC4DynamicArray>(arc4ElemType);
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

	case Type::Category::Function:
	{
		auto const* funcType = dynamic_cast<FunctionType const*>(_solType);
		if (funcType && (funcType->kind() == FunctionType::Kind::Internal
			|| funcType->kind() == FunctionType::Kind::External
			|| funcType->kind() == FunctionType::Kind::DelegateCall))
		{
			// Internal function pointers: uint64 (dispatch ID)
			// External function pointers: bytes[12] (appId 8 + selector 4)
			if (funcType->kind() == FunctionType::Kind::Internal)
				result = awst::WType::uint64Type();
			else
				result = createType<awst::BytesWType>(12);
		}
		else
			result = awst::WType::uint64Type(); // default for other function kinds
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
		return createType<awst::ARC4UIntN>(256);
	if (_type == awst::WType::boolType())
		return awst::WType::arc4BoolType();
	if (_type == awst::WType::accountType())
		return createType<awst::ARC4StaticArray>(arc4Byte, 32);
	if (_type == awst::WType::bytesType())
		return createType<awst::ARC4DynamicArray>(arc4Byte, std::string("byte[]"));
	if (_type == awst::WType::stringType())
		return createType<awst::ARC4DynamicArray>(arc4Byte, std::string("string"));

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

	// Use AST ID for cache key to disambiguate same-named structs
	// from different scopes (e.g. Pool.SwapParams vs IPoolManager.SwapParams)
	std::string cacheKey = "struct:" + std::to_string(structDef.id());
	auto it = m_cache.find(cacheKey);
	if (it != m_cache.end())
		return it->second;

	// Recursion guard: if we're already mapping this struct (recursive
	// reference like `struct R { R[] children; }`), return a placeholder
	// to break the cycle. ARC4 has no cycle support, so the recursive
	// field becomes a bytes blob — semantics will be wrong but the
	// compiler won't infinite-recurse into a stack overflow.
	if (m_inProgressStructs.count(structDef.id()))
		return awst::WType::bytesType();
	m_inProgressStructs.insert(structDef.id());

	// Build ARC4Struct with fields in declaration order
	std::vector<std::pair<std::string, awst::WType const*>> fields;

	for (auto const& member: structDef.members())
	{
		if (member->type()->category() == solidity::frontend::Type::Category::Mapping)
		{
			// Mapping fields cannot be ARC4-encoded, but must still appear
			// in the struct type so that FieldExpression accesses are valid.
			// Use bytes as placeholder — the actual mapping data is stored
			// in separate box storage keyed by the mapping name.
			fields.emplace_back(member->name(), awst::WType::bytesType());
			continue;
		}
		auto const* arc4Type = mapSolTypeToARC4(member->type());
		fields.emplace_back(member->name(), arc4Type);
	}

	auto* result = createType<awst::ARC4Struct>(
		name,
		std::move(fields),
		/*_frozen=*/true  // Solidity structs are value types (memory copies)
	);

	m_cache[cacheKey] = result;
	m_inProgressStructs.erase(structDef.id());
	return result;
}

awst::WType const* TypeMapper::mapSolTypeToARC4(solidity::frontend::Type const* _solType)
{
	// Unwrap UserDefinedValueType to underlying type
	if (auto const* udvt = dynamic_cast<solidity::frontend::UserDefinedValueType const*>(_solType))
		_solType = &udvt->underlyingType();

	// Preserve exact bit width for integers (don't upcast uint8→uint64)
	if (auto const* intType = dynamic_cast<solidity::frontend::IntegerType const*>(_solType))
	{
		unsigned bits = intType->numBits();
		if (intType->isSigned())
		{
			std::string alias = "int" + std::to_string(bits);
			return createType<awst::ARC4UIntN>(static_cast<int>(bits), alias);
		}
		return createType<awst::ARC4UIntN>(static_cast<int>(bits));
	}

	// Enums → ARC4UIntN(8) (enums are always uint8 in Solidity ABI)
	if (auto const* enumType = dynamic_cast<solidity::frontend::EnumType const*>(_solType))
		return createType<awst::ARC4UIntN>(8);

	// Default: map through raw type → ARC4
	return mapToARC4Type(map(_solType));
}

// ── solc adapter helpers ──

std::string TypeMapper::abiSignatureForFunction(
	solidity::frontend::FunctionDefinition const& _fd)
{
	auto const* ft = _fd.functionType(false);
	if (!ft) return {};
	try { return ft->externalSignature(); }
	catch (...) { return {}; }
}

std::string TypeMapper::abiSignatureForFunction(
	solidity::frontend::FunctionType const& _ft)
{
	try { return _ft.externalSignature(); }
	catch (...) { return {}; }
}

std::string TypeMapper::abiTypeName(solidity::frontend::Type const& _t)
{
	try { return _t.canonicalName(); }
	catch (...) { return {}; }
}

TypeMapper::ImplicitConvert TypeMapper::canImplicitlyConvert(
	solidity::frontend::Type const& _from,
	solidity::frontend::Type const& _to)
{
	auto br = _from.isImplicitlyConvertibleTo(_to);
	if (br) return {true, {}};
	return {false, br.message()};
}

solidity::frontend::FunctionDefinition const* TypeMapper::resolveVirtual(
	solidity::frontend::ContractDefinition const& _mostDerived,
	solidity::frontend::FunctionDefinition const& _baseFn,
	bool _superLookup)
{
	try
	{
		// _searchStart: nullptr → look from _mostDerived. Non-null →
		// start AFTER that contract in the linearization (super lookup).
		solidity::frontend::ContractDefinition const* searchStart = nullptr;
		if (_superLookup)
			searchStart = _baseFn.annotation().contract;
		auto const& resolved = _baseFn.resolveVirtual(_mostDerived, searchStart);
		return &resolved;
	}
	catch (...)
	{
		return nullptr;
	}
}

} // namespace puyasol::builder
