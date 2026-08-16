#include "builder/sol-types/TypeMapper.h"
#include "builder/sol-types/FunctionPointerKind.h"
#include "Logger.h"

#include <libsolidity/ast/AST.h>

namespace puyasol::builder
{

awst::WType const* TypeMapper::map(solidity::frontend::Type const* _solType)
{
	using namespace solidity::frontend;

	if (!_solType)
		return awst::WType::voidType();

	std::string const typeStr = _solType->toString(true);
	auto it = m_solTypeCache.find(_solType);
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
						// mapSolTypeToARC4 preserves exact bit widths (avoids uint8→uint64→arc4.uint64).
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
		result = awst::WType::accountType();
		break;

	case Type::Category::Enum:
		result = awst::WType::uint64Type();
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
	{
		auto const* ratType = dynamic_cast<RationalNumberType const*>(_solType);
		if (ratType)
		{
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
		// External/DelegateCall carries appId + routing selector. The opt-in
		// selector mode adds the Solidity selector as a separate field.
		if (isExternalFunctionPointer(funcType))
			result = createType<awst::BytesWType>(
				externalFunctionPointerWidth(m_profile));
		else
			result = awst::WType::uint64Type();
		break;
	}

	default:
	{
		// Meta-types carry no runtime value — type(X), modules, abi/block/msg magic,
		// modifiers, inaccessible-dynamic; real ops route through dedicated paths — and
		// array slices (`x[a:b]`) are modeled as bytes. Both keep the bytes fallback.
		// Any OTHER unmapped, value-carrying type reaching here would silently become
		// bytes and diverge from EVM, so refuse to compile (was a silent warning).
		auto const cat = _solType->category();
		if (cat == Type::Category::TypeType || cat == Type::Category::Modifier
			|| cat == Type::Category::Magic || cat == Type::Category::Module
			|| cat == Type::Category::InaccessibleDynamic
			|| cat == Type::Category::ArraySlice)
			result = awst::WType::bytesType();
		else
		{
			Logger::instance().error(
				"unsupported type '" + typeStr + "' — no AVM mapping; refusing a "
				"silent bytes fallback (would diverge from EVM semantics)");
			result = awst::WType::bytesType(); // keep building until the error aborts
		}
		break;
	}
	}

	if (result)
		m_solTypeCache[_solType] = result;
	else
		result = awst::WType::voidType();

	return result;
}

awst::WType const* TypeMapper::mapToARC4Type(awst::WType const* _type)
{
	if (!_type)
		return nullptr;

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

	// ReferenceArray → ARC4StaticArray (if sized) or ARC4DynamicArray
	if (_type->kind() == awst::WTypeKind::ReferenceArray)
	{
		auto const* refArr = static_cast<awst::ReferenceArray const*>(_type);
		auto const* arc4Elem = mapToARC4Type(refArr->elementType());
		if (refArr->arraySize().has_value())
			return remember(createType<awst::ARC4StaticArray>(
				arc4Elem, refArr->arraySize().value()));
		return remember(createType<awst::ARC4DynamicArray>(arc4Elem));
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

awst::WType const* TypeMapper::mapStruct(solidity::frontend::StructType const* _structType)
{
	if (!_structType)
		return awst::WType::voidType();

	auto const& structDef = _structType->structDefinition();
	std::string name = structDef.name();

	// Cache by AST ID to disambiguate same-named structs from different scopes.
	std::string cacheKey = "struct:" + std::to_string(structDef.id());
	auto it = m_namedTypeCache.find(cacheKey);
	if (it != m_namedTypeCache.end())
		return it->second;

	// Recursion guard for `struct R { R[] children; }` cycles (ARC4 has no cycle
	// support). On re-entry, return a FIXED PROJECTION of the struct — its fields,
	// but recursive (in-progress) array/mapping fields stubbed to a bytes pointer.
	// Keeps the field SHAPE (so element access like `s.x[i].v` still resolves),
	// unlike a bare bytes blob, and stays non-recursive so puya accepts it.
	// (solc's `structDef.annotation().recursive` would short-circuit the whole
	// struct to bytes, losing outer non-cycling fields — per-cycle is the right model.)
	if (m_inProgressStructs.count(structDef.id()))
	{
		std::string projKey = "structproj:" + std::to_string(structDef.id());
		auto pit = m_namedTypeCache.find(projKey);
		if (pit != m_namedTypeCache.end())
			return pit->second;
		std::vector<std::pair<std::string, awst::WType const*>> projFields;
		for (auto const& member: structDef.members())
		{
			bool recursiveField =
				member->type()->category() == solidity::frontend::Type::Category::Mapping;
			if (auto const* arr =
					dynamic_cast<solidity::frontend::ArrayType const*>(member->type());
				arr && !arr->isByteArrayOrString())
				if (auto const* es =
						dynamic_cast<solidity::frontend::StructType const*>(arr->baseType());
					es && m_inProgressStructs.count(es->structDefinition().id()))
					recursiveField = true;
			projFields.emplace_back(
				member->name(),
				recursiveField ? awst::WType::bytesType() : mapSolTypeToARC4(member->type()));
		}
		auto* proj = createType<awst::ARC4Struct>(name + "__rec", std::move(projFields),
			/*_frozen=*/false);
		m_namedTypeCache[projKey] = proj;
		return proj;
	}
	m_inProgressStructs.insert(structDef.id());

	std::vector<std::pair<std::string, awst::WType const*>> fields;

	for (auto const& member: structDef.members())
	{
		if (member->type()->category() == solidity::frontend::Type::Category::Mapping)
		{
			// Mapping fields: bytes placeholder so FieldExpression accesses work;
			// actual mapping data lives in separate box storage.
			fields.emplace_back(member->name(), awst::WType::bytesType());
			continue;
		}
		auto const* arc4Type = mapSolTypeToARC4(member->type());
		fields.emplace_back(member->name(), arc4Type);
	}

	auto* result = createType<awst::ARC4Struct>(
		name,
		std::move(fields),
		/*_frozen=*/false  // Mutable: struct-field writes must not hit puya's "immutable"
		                   // rejection. Value-type semantics enforced by puya-sol's
		                   // copy-on-write handlers, not this flag.
	);

	m_namedTypeCache[cacheKey] = result;
	m_inProgressStructs.erase(structDef.id());
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

	// Preserve exact bit width (don't upcast uint8→uint64).
	if (auto const* intType = dynamic_cast<solidity::frontend::IntegerType const*>(_solType))
	{
		unsigned bits = intType->numBits();
		if (intType->isSigned())
		{
			std::string alias = "int" + std::to_string(bits);
			result = createType<awst::ARC4UIntN>(static_cast<int>(bits), alias);
		}
		else
		{
			if (bits == 8 && m_arc4ByteType)
				result = m_arc4ByteType;
			else
				result = createType<awst::ARC4UIntN>(static_cast<int>(bits));
			if (bits == 8)
				m_arc4ByteType = result;
		}
	}

	// Enums → ARC4UIntN(8) (always uint8 in Solidity ABI).
	else if (dynamic_cast<solidity::frontend::EnumType const*>(_solType))
	{
		if (!m_arc4ByteType)
			m_arc4ByteType = createType<awst::ARC4UIntN>(8);
		result = m_arc4ByteType;
	}
	else
		result = mapToARC4Type(map(_solType));

	m_solArc4Cache.emplace(cacheKey, result);
	return result;
}

} // namespace puyasol::builder
