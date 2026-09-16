#include "builder/types/TypeMapper.h"
#include "builder/types/FunctionPointerKind.h"
#include "builder/codec/Arc4Defaults.h"
#include "builder/solc/StorageRefPointer.h"
#include "Logger.h"

#include <libsolidity/ast/AST.h>
#include <libsolidity/ast/TypeProvider.h>
#include "builder/types/SolIntType.h"

namespace puyasol::builder
{

namespace
{
/// Value representations erase array/struct locations, including tuple components.
/// Function signatures retain their semantic parameter/return locations; these are
/// callable types, not value buffers. Mappings retain their storage-only identity.
/// Solc owns all nominal identities and recursively relocates array base types.
solidity::frontend::Type const* representationType(solidity::frontend::Type const* _type)
{
	using namespace solidity::frontend;
	if (auto const* tuple = dynamic_cast<TupleType const*>(_type))
	{
		TypePointers components;
		for (auto const* component: tuple->components())
			components.push_back(representationType(component));
		return TypeProvider::tuple(std::move(components));
	}
	// Not every ReferenceType is relocatable: solc's calldata ArraySliceType
	// deliberately asserts in copyForLocation(). Only value buffers normalize.
	if (dynamic_cast<ArrayType const*>(_type) || dynamic_cast<StructType const*>(_type))
		return TypeProvider::withLocationIfReference(DataLocation::Memory, _type);
	return _type;
}

bool reachesStruct(
	solidity::frontend::Type const* _type,
	int64_t _root,
	std::set<solidity::frontend::Type const*>& _visiting)
{
	using namespace solidity::frontend;
	if (!_type || !_visiting.insert(_type).second)
		return false;
	if (auto const* structure = dynamic_cast<StructType const*>(_type))
	{
		if (structure->structDefinition().id() == _root)
			return true;
		for (auto const& member: structure->structDefinition().members())
			if (reachesStruct(member->type(), _root, _visiting))
				return true;
		return false;
	}
	if (auto const* array = dynamic_cast<ArrayType const*>(_type))
		return reachesStruct(array->baseType(), _root, _visiting);
	return false;
}
}

awst::WType const* TypeMapper::mapArray(
	solidity::frontend::Type const* _solType, bool _projection)
{
	using namespace solidity::frontend;
	auto const* array = dynamic_cast<ArrayType const*>(_solType);
	if (!array || array->isString()) return awst::WType::stringType();
	if (array->isByteArrayOrString()) return awst::WType::bytesType();
	auto const* structure = dynamic_cast<StructType const*>(array->baseType());
	// Only a recursive struct's cycling fields need projected array elements.
	// Standalone array values retain their full elements. Neither projected
	// structs nor projected arrays enter the ordinary Solidity/ARC4 caches.
	auto const* element = _projection && structure && structure->recursive()
		? mapStruct(structure, true)
		: _projection && dynamic_cast<ArrayType const*>(array->baseType())
			? mapArray(array->baseType(), true) : mapSolTypeToARC4(array->baseType());
	awst::WType const* result;
	if (array->isDynamicallySized())
		result = createType<awst::ARC4DynamicArray>(element);
	else
	{
		result = createType<awst::ARC4StaticArray>(element,
			checkedSize<int64_t>(array->length(), "Solidity array length"));
		computeEncodedElementSize(result).fixedBytes<int>();
	}
	m_aggregateSources.emplace(result, array);
	return result;
}

namespace
{
using namespace solidity::frontend;

awst::WType const* mapRationalCategory(TypeMapper& _tm, Type const* _solType)
{
	auto const* ratType = dynamic_cast<RationalNumberType const*>(_solType);
	if (!ratType)
		return awst::WType::biguintType();
	auto const* mobileType = ratType->mobileType();
	if (mobileType)
		return _tm.map(mobileType);
	return awst::WType::biguintType();
}

awst::WType const* mapTupleCategory(TypeMapper& _tm, Type const* _solType)
{
	auto const* tupleType = dynamic_cast<TupleType const*>(_solType);
	if (!tupleType)
		return nullptr;
	if (tupleType->components().empty())
	{
		// Empty tuple = void (e.g. return type of void function call)
		return awst::WType::voidType();
	}
	std::vector<awst::WType const*> types;
	for (auto const& comp: tupleType->components())
		types.push_back(_tm.map(comp));
	return _tm.createType<awst::WTuple>(std::move(types));
}

awst::WType const* mapFunctionCategory(TypeMapper& _tm, Type const* _solType)
{
	auto const* funcType = dynamic_cast<FunctionType const*>(_solType);
	// External/DelegateCall carries appId + routing selector. The opt-in
	// selector mode adds the Solidity selector as a separate field.
	if (isExternalFunctionPointer(funcType))
		return _tm.createType<awst::BytesWType>(
			externalFunctionPointerWidth(_tm.profile()));
	return awst::WType::uint64Type();
}

/// Meta-types carry no runtime value — type(X), modules, abi/block/msg magic, modifiers, inaccessible-dynamic; real ops route …
awst::WType const* mapFallbackCategory(Type const* _solType, std::string const& _typeStr)
{
	auto const cat = _solType->category();
	if (cat == Type::Category::TypeType || cat == Type::Category::Modifier
		|| cat == Type::Category::Magic || cat == Type::Category::Module
		|| cat == Type::Category::InaccessibleDynamic)
		return awst::WType::bytesType();
	Logger::instance().error(
		"unsupported type '" + _typeStr + "' — no AVM mapping; refusing a "
		"silent bytes fallback (would diverge from EVM semantics)");
	return awst::WType::bytesType(); // keep building until the error aborts
}
} // namespace

awst::WType const* TypeMapper::map(solidity::frontend::Type const* _solType)
{
	using namespace solidity::frontend;

	if (!_solType)
		return awst::WType::voidType();

	std::string const cacheKey = representationType(_solType)->identifier();
	auto it = m_solTypeCache.find(cacheKey);
	if (it != m_solTypeCache.end())
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
		result = mapArray(_solType);
		break;
	case Type::Category::ArraySlice:
		result = mapArray(
			&static_cast<ArraySliceType const*>(_solType)->arrayType());
		break;

	case Type::Category::Struct:
	{
		auto const* structType = dynamic_cast<StructType const*>(_solType);
		if (structType)
			result = mapStruct(structType);
		break;
	}

	case Type::Category::Contract:
		result = awst::WType::accountType();
		break;

	case Type::Category::Enum:
		// Assembly can dirty the complete word. Preserve it until solc's enum
		// validation boundary; ABI and packed storage still use the solc width.
		result = awst::WType::biguintType();
		break;

	case Type::Category::UserDefinedValueType:
	{
		// UDVTs (e.g. `type Fr is uint256`) map to their underlying type.
		auto const* udvt = dynamic_cast<UserDefinedValueType const*>(_solType);
		if (udvt)
			result = map(&udvt->underlyingType());
		break;
	}

	case Type::Category::Mapping:
		// Handled at storage level; bytes placeholder here.
		result = awst::WType::bytesType();
		break;

	case Type::Category::RationalNumber:
		result = mapRationalCategory(*this, _solType);
		break;

	case Type::Category::Tuple:
		result = mapTupleCategory(*this, _solType);
		break;

	case Type::Category::Function:
		result = mapFunctionCategory(*this, _solType);
		break;

	default:
		result = mapFallbackCategory(_solType, _solType->toString(true));
		break;
	}

	if (result)
	{
		m_solTypeCache[cacheKey] = result;
	}
	else
		result = awst::WType::voidType();

	return result;
}

awst::WType const* TypeMapper::tryMapStorageRepresentation(solidity::frontend::Type const* _solType)
{
	try { return map(_solType); }
	catch (SizeError const&) { return nullptr; }
}

bool TypeMapper::isBoxKeyedStorageRef(solidity::frontend::Type const* _solType)
{
	if (containsMappingType(_solType)) return true;
	if (auto const* structure = dynamic_cast<solidity::frontend::StructType const*>(_solType))
	{
		if (hasDynamicStorageShape(structure)) return true;
		auto id = structure->structDefinition().id();
		if (m_analysis.boxKeyedStructs.contains(id) || m_analysis.refPassedStructs.contains(id))
			return true;
		// At least 128 encoded bytes cannot fit global state with any nonempty
		// key. Solc's EVM-slot upper bound does not describe this representation.
		auto const* representation = tryMapStorageRepresentation(structure);
		return !representation
			|| computeEncodedElementSize(representation).fixedBytes().value_or(0) >= 128;
	}
	if (auto const* array = dynamic_cast<solidity::frontend::ArrayType const*>(_solType))
		return !array->isByteArrayOrString() && hasDynamicStorageShape(array);
	return false;
}

awst::WType const* TypeMapper::mapToARC4Type(awst::WType const* _type)
{
	if (!_type)
		return nullptr;

	if (isArc4EncodedType(_type)) return _type;

	if (auto const found = m_arc4Cache.find(_type); found != m_arc4Cache.end())
		return found->second;
	auto remember = [&](awst::WType const* _result) {
		m_arc4Cache.emplace(_type, _result);
		return _result;
	};
	auto arc4Byte = [&]() {
		if (!m_arc4ByteType)
			m_arc4ByteType = createType<awst::ARC4UIntN>(8);
		return m_arc4ByteType;
	};

	if (_type == awst::WType::uint64Type())
		return remember(createType<awst::ARC4UIntN>(64));
	if (_type == awst::WType::biguintType())
		return remember(createType<awst::ARC4UIntN>(256));
	if (_type == awst::WType::boolType())
		return remember(awst::WType::arc4BoolType());
	if (_type == awst::WType::accountType())
		return remember(createType<awst::ARC4StaticArray>(
			arc4Byte(), 32, std::string("address")));
	if (_type == awst::WType::bytesType())
		return remember(createType<awst::ARC4DynamicArray>(
			arc4Byte(), std::string("byte[]")));
	if (_type == awst::WType::stringType())
		return remember(createType<awst::ARC4DynamicArray>(
			arc4Byte(), std::string("string")));

	if (_type->kind() == awst::WTypeKind::Bytes)
	{
		auto const* bytesType = static_cast<awst::BytesWType const*>(_type);
		if (bytesType->length().has_value())
		{
			auto len = bytesType->length().value();
			return remember(createType<awst::ARC4StaticArray>(
				arc4Byte(), len, "byte[" + std::to_string(len) + "]"));
		}
		return remember(createType<awst::ARC4DynamicArray>(arc4Byte()));
	}

	// WTuple → ARC4Tuple
	if (_type->kind() == awst::WTypeKind::WTuple)
	{
		auto const* tupleType = static_cast<awst::WTuple const*>(_type);
		std::vector<awst::WType const*> arc4Types;
		for (auto const* t: tupleType->types())
			arc4Types.push_back(mapToARC4Type(t));
		return remember(createType<awst::ARC4Tuple>(std::move(arc4Types)));
	}

	return remember(_type); // best effort
}

awst::WType const* TypeMapper::mapStruct(
	solidity::frontend::StructType const* _structType, bool _projection)
{
	if (!_structType) return awst::WType::voidType();
	auto const& definition = _structType->structDefinition();
	auto& cache = _projection ? m_structProjections : m_structTypes;
	if (auto it = cache.find(definition.id()); it != cache.end()) return it->second;

	if (!_projection && !profile().evmStorageLayout)
		if (auto const* inner = transparentMappingWrapper(_structType))
		{
			auto const* innerType = dynamic_cast<awst::ARC4Struct const*>(mapStruct(inner));
			auto* result = createType<awst::ARC4Struct>(definition.name(), innerType->fields(), false);
			cache.emplace(definition.id(), result);
			m_aggregateSources.emplace(result, solcAggregateFor(innerType));
			return result;
		}

	std::vector<std::pair<std::string, awst::WType const*>> fields;
	for (auto const& member: definition.members())
	{
		std::set<solidity::frontend::Type const*> visiting;
		bool const opaque = member->type()->category() == solidity::frontend::Type::Category::Mapping
			|| (_projection && reachesStruct(member->type(), definition.id(), visiting));
		visiting.clear();
		bool const projectedArray = dynamic_cast<solidity::frontend::ArrayType const*>(member->type())
			&& reachesStruct(member->type(), definition.id(), visiting);
		fields.emplace_back(member->name(), opaque ? awst::WType::bytesType()
			: projectedArray ? mapArray(member->type(), true) : mapSolTypeToARC4(member->type()));
	}
	auto* result = createType<awst::ARC4Struct>(
		definition.name() + (_projection ? "__rec" : ""), std::move(fields), false);
	cache.emplace(definition.id(), result);
	m_aggregateSources.emplace(result, _structType);
	return result;
}

awst::WType const* TypeMapper::mapSolTypeToARC4(solidity::frontend::Type const* _solType)
{
	if (!_solType)
		return nullptr;
	auto const* cacheKey = _solType;
	if (auto const found = m_solArc4Cache.find(cacheKey);
		found != m_solArc4Cache.end())
		return found->second;

	if (auto const* udvt = dynamic_cast<solidity::frontend::UserDefinedValueType const*>(_solType))
		_solType = &udvt->underlyingType();
	awst::WType const* result = nullptr;

	// Exact widths, signedness, UDVT underlying types and enum encoding types
	// come from solc, not a duplicated enum-width or source-name convention.
	if (auto const integer = SolIntType::fromSolOrEnum(_solType))
	{
		if (!integer->isSigned && integer->bits == 8 && m_arc4ByteType)
			result = m_arc4ByteType;
		else
			result = createType<awst::ARC4UIntN>(static_cast<int>(integer->bits),
				integer->isSigned ? "int" + std::to_string(integer->bits) : "");
		if (!integer->isSigned && integer->bits == 8) m_arc4ByteType = result;
	}
	else
		result = mapToARC4Type(map(_solType));

	m_solArc4Cache.emplace(cacheKey, result);
	return result;
}

} // namespace puyasol::builder
