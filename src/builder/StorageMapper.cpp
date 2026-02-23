#include "builder/StorageMapper.h"

#include <liblangutil/SourceLocation.h>

#include <set>

namespace puyasol::builder
{

awst::SourceLocation StorageMapper::makeLoc(
	solidity::langutil::SourceLocation const& _solLoc,
	std::string const& _file
)
{
	awst::SourceLocation loc;
	loc.file = _file;
	loc.line = _solLoc.start >= 0 ? _solLoc.start : 0;
	loc.endLine = _solLoc.end >= 0 ? _solLoc.end : 0;
	return loc;
}

std::shared_ptr<awst::BytesConstant> StorageMapper::makeKeyExpr(
	std::string const& _name,
	awst::SourceLocation const& _loc,
	awst::AppStorageKind _kind
)
{
	auto key = std::make_shared<awst::BytesConstant>();
	key->sourceLocation = _loc;
	key->wtype = (_kind == awst::AppStorageKind::Box)
		? awst::WType::boxKeyType()
		: awst::WType::stateKeyType();
	key->encoding = awst::BytesEncoding::Utf8;
	key->value = std::vector<uint8_t>(_name.begin(), _name.end());
	return key;
}

std::shared_ptr<awst::Expression> StorageMapper::makeDefaultValue(
	awst::WType const* _type,
	awst::SourceLocation const& _loc
)
{
	if (!_type)
	{
		auto val = std::make_shared<awst::BytesConstant>();
		val->sourceLocation = _loc;
		val->wtype = awst::WType::bytesType();
		val->encoding = awst::BytesEncoding::Base16;
		val->value = {};
		return val;
	}

	// Bool → BoolConstant (not IntegerConstant, which only accepts uint64/biguint/ARC4UIntN)
	if (_type == awst::WType::boolType())
	{
		auto val = std::make_shared<awst::BoolConstant>();
		val->sourceLocation = _loc;
		val->wtype = awst::WType::boolType();
		val->value = false;
		return val;
	}

	// Integer types → IntegerConstant
	if (_type == awst::WType::uint64Type())
	{
		auto val = std::make_shared<awst::IntegerConstant>();
		val->sourceLocation = _loc;
		val->wtype = awst::WType::uint64Type();
		val->value = "0";
		return val;
	}
	if (_type == awst::WType::biguintType())
	{
		auto val = std::make_shared<awst::IntegerConstant>();
		val->sourceLocation = _loc;
		val->wtype = awst::WType::biguintType();
		val->value = "0";
		return val;
	}
	if (_type->kind() == awst::WTypeKind::ARC4UIntN)
	{
		auto val = std::make_shared<awst::IntegerConstant>();
		val->sourceLocation = _loc;
		val->wtype = _type;
		val->value = "0";
		return val;
	}

	// Tuple → TupleExpression with component defaults (recursive)
	if (_type->kind() == awst::WTypeKind::WTuple)
	{
		auto const* tupleType = static_cast<awst::WTuple const*>(_type);
		auto tuple = std::make_shared<awst::TupleExpression>();
		tuple->sourceLocation = _loc;
		tuple->wtype = _type;
		for (auto const* componentType: tupleType->types())
			tuple->items.push_back(makeDefaultValue(componentType, _loc));
		return tuple;
	}

	// Everything else (bytes, string, account, ARC4 types, etc.) → BytesConstant
	// Use the actual type (not bytesType()) so the wtype matches the storage field wtype.
	// In Puya, account/string/bytes/ARC4 types are all bytes-backed and can hold BytesConstant.
	auto val = std::make_shared<awst::BytesConstant>();
	val->sourceLocation = _loc;
	val->wtype = _type;
	val->encoding = awst::BytesEncoding::Base16;

	// For fixed-size types, create zero bytes of the correct length
	if (_type == awst::WType::accountType())
		val->value = std::vector<uint8_t>(32, 0); // 32-byte zero address
	else if (auto const* bytesType = dynamic_cast<awst::BytesWType const*>(_type))
	{
		if (bytesType->length().has_value())
			val->value = std::vector<uint8_t>(static_cast<size_t>(bytesType->length().value()), 0);
		else
			val->value = {};
	}
	else
		val->value = {};

	return val;
}

bool StorageMapper::shouldUseBoxStorage(solidity::frontend::VariableDeclaration const& _var)
{
	auto const* type = _var.type();
	if (!type)
		return false;

	// Mappings always use box storage
	if (type->category() == solidity::frontend::Type::Category::Mapping)
		return true;

	// Dynamic arrays also use box storage
	if (auto const* arrType = dynamic_cast<solidity::frontend::ArrayType const*>(type))
	{
		if (arrType->isDynamicallySized())
			return true;
	}

	return false;
}

std::vector<awst::AppStorageDefinition> StorageMapper::mapStateVariables(
	solidity::frontend::ContractDefinition const& _contract,
	std::string const& _sourceFile
)
{
	std::vector<awst::AppStorageDefinition> defs;
	std::set<std::string> seen; // avoid duplicates from inheritance

	// Iterate all contracts in linearization order (includes base contracts)
	for (auto const* base: _contract.annotation().linearizedBaseContracts)
	{
		for (auto const* var: base->stateVariables())
		{
			if (var->isConstant())
				continue;
			// On Algorand, immutable vars are stored in global state (no code-embedded storage)
			if (seen.count(var->name()))
				continue;
			seen.insert(var->name());

			awst::AppStorageDefinition def;
			def.sourceLocation = makeLoc(var->location(), _sourceFile);
			def.memberName = var->name();

			if (shouldUseBoxStorage(*var))
			{
				def.storageKind = awst::AppStorageKind::Box;

				// For mappings, the value type is the storage type.
			// For nested mappings (e.g. mapping(address => mapping(address => bool))),
			// unwrap recursively to find the final non-mapping value type.
			if (auto const* mappingType = dynamic_cast<solidity::frontend::MappingType const*>(var->type()))
			{
				solidity::frontend::Type const* valueType = mappingType->valueType();
				while (auto const* nestedMapping = dynamic_cast<solidity::frontend::MappingType const*>(valueType))
					valueType = nestedMapping->valueType();
				def.storageWType = m_typeMapper.map(valueType);
				def.isMap = true;
			}
				else if (auto const* arrType = dynamic_cast<solidity::frontend::ArrayType const*>(var->type()))
				{
					// Dynamic arrays → box map with element type as storage type
					def.storageWType = m_typeMapper.map(arrType->baseType());
					def.isMap = true;

					// Also create a global state entry for the array length
					awst::AppStorageDefinition lenDef;
					lenDef.sourceLocation = def.sourceLocation;
					lenDef.memberName = var->name() + "_length";
					lenDef.storageKind = awst::AppStorageKind::AppGlobal;
					lenDef.storageWType = awst::WType::uint64Type();
					lenDef.key = makeKeyExpr(var->name() + "_length", lenDef.sourceLocation, lenDef.storageKind);
					defs.push_back(std::move(lenDef));
				}
				else
				{
					def.storageWType = m_typeMapper.map(var->type());
				}
			}
			else
			{
				def.storageKind = awst::AppStorageKind::AppGlobal;
				def.storageWType = m_typeMapper.map(var->type());
			}

			def.key = makeKeyExpr(var->name(), def.sourceLocation, def.storageKind);
			defs.push_back(std::move(def));
		}
	}

	return defs;
}

std::shared_ptr<awst::Expression> StorageMapper::createStateRead(
	std::string const& _varName,
	awst::WType const* _type,
	awst::AppStorageKind _kind,
	awst::SourceLocation const& _loc
)
{
	auto key = makeKeyExpr(_varName, _loc, _kind);

	switch (_kind)
	{
	case awst::AppStorageKind::AppGlobal:
	{
		auto expr = std::make_shared<awst::AppStateExpression>();
		expr->sourceLocation = _loc;
		expr->wtype = _type;
		expr->key = key;
		expr->existsAssertionMessage = "check " + _varName + " exists";
		return expr;
	}
	case awst::AppStorageKind::Box:
	{
		// Use StateGet with a default value so that missing boxes return the
		// Solidity default (0/false/empty) instead of asserting existence.
		auto boxExpr = std::make_shared<awst::BoxValueExpression>();
		boxExpr->sourceLocation = _loc;
		boxExpr->wtype = _type;
		boxExpr->key = key;
		boxExpr->existsAssertionMessage = std::nullopt;

		auto defaultVal = makeDefaultValue(_type, _loc);

		auto stateGet = std::make_shared<awst::StateGet>();
		stateGet->sourceLocation = _loc;
		stateGet->wtype = _type;
		stateGet->field = boxExpr;
		stateGet->defaultValue = defaultVal;
		return stateGet;
	}
	default:
	{
		auto expr = std::make_shared<awst::AppStateExpression>();
		expr->sourceLocation = _loc;
		expr->wtype = _type;
		expr->key = key;
		return expr;
	}
	}
}

std::shared_ptr<awst::Expression> StorageMapper::createStateWrite(
	std::string const& _varName,
	std::shared_ptr<awst::Expression> _value,
	awst::WType const* _type,
	awst::AppStorageKind _kind,
	awst::SourceLocation const& _loc
)
{
	auto key = makeKeyExpr(_varName, _loc, _kind);

	std::shared_ptr<awst::Expression> target;
	switch (_kind)
	{
	case awst::AppStorageKind::AppGlobal:
	{
		auto expr = std::make_shared<awst::AppStateExpression>();
		expr->sourceLocation = _loc;
		expr->wtype = _type;
		expr->key = key;
		target = expr;
		break;
	}
	case awst::AppStorageKind::Box:
	{
		auto expr = std::make_shared<awst::BoxValueExpression>();
		expr->sourceLocation = _loc;
		expr->wtype = _type;
		expr->key = key;
		target = expr;
		break;
	}
	default:
	{
		auto expr = std::make_shared<awst::AppStateExpression>();
		expr->sourceLocation = _loc;
		expr->wtype = _type;
		expr->key = key;
		target = expr;
		break;
	}
	}

	auto assign = std::make_shared<awst::AssignmentExpression>();
	assign->sourceLocation = _loc;
	assign->wtype = _type;
	assign->target = target;
	assign->value = std::move(_value);
	return assign;
}

} // namespace puyasol::builder
