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
				// Dynamic array
				awst::WType const* elemType = map(arrType->baseType());
				// Puya only supports reference arrays with fixed-size elements
				bool isFixed = elemType
					&& elemType != awst::WType::biguintType()
					&& elemType != awst::WType::stringType()
					&& elemType != awst::WType::bytesType();
				if (isFixed)
					result = createType<awst::ReferenceArray>(elemType, false);
				else
				{
					Logger::instance().warning("array of dynamic-size elements not supported, using bytes");
					result = awst::WType::bytesType();
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
			std::vector<awst::WType const*> types;
			for (auto const& comp: tupleType->components())
				types.push_back(map(comp));
			result = createType<awst::WTuple>(std::move(types));
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

	// Build named tuple for the struct
	std::vector<awst::WType const*> fieldTypes;
	std::vector<std::string> fieldNames;

	for (auto const& member: structDef.members())
	{
		fieldNames.push_back(member->name());
		fieldTypes.push_back(map(member->type()));
	}

	auto* result = createType<awst::WTuple>(
		std::move(fieldTypes),
		std::optional<std::vector<std::string>>(std::move(fieldNames))
	);

	m_cache["struct:" + name] = result;
	return result;
}

} // namespace puyasol::builder
