#include "builder/storage/StorageMapper.h"
#include "builder/sol-types/TypeCoercion.h"

#include "Logger.h"

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
	return awst::makeUtf8BytesConstant(
		_name, _loc,
		_kind == awst::AppStorageKind::Box
			? awst::WType::boxKeyType()
			: awst::WType::stateKeyType());
}

std::shared_ptr<awst::Expression> StorageMapper::makeDefaultValue(
	awst::WType const* _type,
	awst::SourceLocation const& _loc
)
{
	return TypeCoercion::makeDefaultValue(_type, _loc);
}

int StorageMapper::computeEncodedElementSize(awst::WType const* _type)
{
	return TypeCoercion::computeEncodedElementSize(_type);
}

bool StorageMapper::shouldUseBoxStorage(solidity::frontend::VariableDeclaration const& _var)
{
	auto const* type = _var.type();
	if (!type)
		return false;

	// Mappings always use box storage
	if (type->category() == solidity::frontend::Type::Category::Mapping)
		return true;

	// Dynamic arrays and dynamic bytes use box storage.
	// String state vars stay in global state (typically short: names, symbols, URIs).
	if (auto const* arrType = dynamic_cast<solidity::frontend::ArrayType const*>(type))
	{
		if (arrType->isDynamicallySized() && !arrType->isString())
			return true;
	}

	// Large values don't fit in AVM global state — promote to box storage.
	// AVM limit: 128 bytes total for key + value. Key = variable name (UTF-8).
	// Large values don't fit in AVM global state (128 bytes for key+value).
	// Use storageSizeUpperBound() (slot count) × 32 for accurate multi-slot sizing.
	try
	{
		auto slotsUpperBound = type->storageSizeUpperBound();
		unsigned estimatedBytes = static_cast<unsigned>(slotsUpperBound) * 32;
		unsigned keyBytes = static_cast<unsigned>(_var.name().size());
		unsigned maxValueBytes = (128 > keyBytes) ? (128 - keyBytes) : 0;
		if (estimatedBytes > maxValueBytes)
			return true;
	}
	catch (...) {}

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
			if (var->referenceLocation() == solidity::frontend::VariableDeclaration::Location::Transient)
				continue;
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
					// Dynamic state array → box-backed ARC4 dynamic array.
					def.storageWType = m_typeMapper.map(arrType);
					def.isMap = false;
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

				if (def.storageWType == awst::WType::stringType())
					Logger::instance().info(
						"string state variable '" + var->name()
						+ "' uses Algorand global state (limited to ~64 bytes)"
					);
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

std::shared_ptr<awst::Expression> StorageMapper::biguintSlotToBtoi(
	std::shared_ptr<awst::Expression> const& _slotExpr,
	awst::SourceLocation const& _loc
)
{
	// reinterpret_cast<bytes>(slotExpr)
	auto castToBytes = awst::makeReinterpretCast(_slotExpr, awst::WType::bytesType(), _loc);

	// len(castToBytes)
	auto lenOp = awst::makeIntrinsicCall("len", awst::WType::uint64Type(), _loc);
	lenOp->stackArgs.push_back(castToBytes);

	// len - 8
	auto sub8 = std::make_shared<awst::UInt64BinaryOperation>();
	sub8->sourceLocation = _loc;
	sub8->wtype = awst::WType::uint64Type();
	sub8->left = std::move(lenOp);
	sub8->op = awst::UInt64BinaryOperator::Sub;
	auto eight = awst::makeIntegerConstant("8", _loc);
	sub8->right = std::move(eight);

	// extract3(castToBytes, len-8, 8)
	auto last8 = awst::makeIntrinsicCall("extract3", awst::WType::bytesType(), _loc);
	last8->stackArgs.push_back(std::move(castToBytes));
	last8->stackArgs.push_back(std::move(sub8));
	auto eight2 = awst::makeIntegerConstant("8", _loc);
	last8->stackArgs.push_back(std::move(eight2));

	// btoi(last8)
	auto btoi = awst::makeIntrinsicCall("btoi", awst::WType::uint64Type(), _loc);
	btoi->stackArgs.push_back(std::move(last8));

	return btoi;
}

} // namespace puyasol::builder
