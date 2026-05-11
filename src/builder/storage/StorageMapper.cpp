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

// ── Multi-box helpers ──

// Multi-box arrays currently support only scalar elements (ARC4UIntN /
// fixed-length bytes) and nested ARC4StaticArray of scalars. Struct
// elements would require copy-on-write through box_extract → modify →
// box_replace logic that's not yet implemented; they fall back to the
// default >32KB-box warning path.
unsigned StorageMapper::arc4StaticArrayElementSize(awst::WType const* _type)
{
	auto const* sa = dynamic_cast<awst::ARC4StaticArray const*>(_type);
	if (!sa) return 0;
	auto const* elem = sa->elementType();
	if (!elem) return 0;
	if (auto const* uintN = dynamic_cast<awst::ARC4UIntN const*>(elem))
		return std::max<unsigned>(1u, static_cast<unsigned>(uintN->n() / 8));
	if (elem->kind() == awst::WTypeKind::Bytes)
	{
		auto const* bw = dynamic_cast<awst::BytesWType const*>(elem);
		if (bw && bw->length().has_value())
			return static_cast<unsigned>(*bw->length());
	}
	if (auto const* nestedSa = dynamic_cast<awst::ARC4StaticArray const*>(elem))
	{
		// Nested ARC4StaticArray of scalar elements: recurse.
		unsigned innerElem = arc4StaticArrayElementSize(nestedSa);
		if (innerElem > 0)
			return innerElem * static_cast<unsigned>(nestedSa->arraySize());
	}
	return 0;
}

uint64_t StorageMapper::arc4StaticArrayTotalBytes(awst::WType const* _type)
{
	auto const* sa = dynamic_cast<awst::ARC4StaticArray const*>(_type);
	if (!sa || sa->arraySize() <= 0) return 0;
	unsigned elemSize = arc4StaticArrayElementSize(_type);
	if (elemSize == 0) return 0;
	return static_cast<uint64_t>(elemSize) * static_cast<uint64_t>(sa->arraySize());
}

bool StorageMapper::isMultiBoxArray(awst::WType const* _type)
{
	uint64_t total = arc4StaticArrayTotalBytes(_type);
	return total > BOX_VALUE_CAPACITY;
}

unsigned StorageMapper::numBoxesForArray(awst::WType const* _type)
{
	uint64_t total = arc4StaticArrayTotalBytes(_type);
	if (total == 0) return 0;  // not a fixed-size array — caller decides
	if (total <= BOX_VALUE_CAPACITY) return 1;
	unsigned elemSize = arc4StaticArrayElementSize(_type);
	if (elemSize == 0) return 1;  // shouldn't happen — guard
	unsigned perBox = elementsPerBox(_type);
	if (perBox == 0) return 1;
	auto const* sa = static_cast<awst::ARC4StaticArray const*>(_type);
	uint64_t totalElems = static_cast<uint64_t>(sa->arraySize());
	return static_cast<unsigned>((totalElems + perBox - 1) / perBox);
}

unsigned StorageMapper::elementsPerBox(awst::WType const* _type)
{
	unsigned elemSize = arc4StaticArrayElementSize(_type);
	if (elemSize == 0) return 0;
	return BOX_VALUE_CAPACITY / elemSize;
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
		// Static outer array whose element is dynamically encoded (e.g. `uint[][2]`):
		// the 2-slot upper bound is misleading — the encoded payload can be arbitrary.
		if (!arrType->isDynamicallySized())
		{
			auto const* baseType = arrType->baseType();
			while (auto const* innerArr = dynamic_cast<solidity::frontend::ArrayType const*>(baseType))
			{
				if (innerArr->isDynamicallySized() && !innerArr->isString())
					return true;
				baseType = innerArr->baseType();
			}
		}
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
		auto expr = awst::makeAppStateExpression(key, _type, _loc);
		expr->existsAssertionMessage = "check " + _varName + " exists";
		return expr;
	}
	case awst::AppStorageKind::Box:
	{
		// Use StateGet with a default value so that missing boxes return the
		// Solidity default (0/false/empty) instead of asserting existence.
		auto boxExpr = awst::makeBoxValueExpression(key, _type, _loc);
		auto defaultVal = makeDefaultValue(_type, _loc);

		auto stateGet = awst::makeStateGet(boxExpr, defaultVal, _type, _loc);
		return stateGet;
	}
	default:
	{
		return awst::makeAppStateExpression(key, _type, _loc);
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
		target = awst::makeAppStateExpression(key, _type, _loc);
		break;
	}
	case awst::AppStorageKind::Box:
	{
		target = awst::makeBoxValueExpression(key, _type, _loc);
		break;
	}
	default:
	{
		target = awst::makeAppStateExpression(key, _type, _loc);
		break;
	}
	}

	return awst::makeAssignmentExpression(target, std::move(_value), _loc, _type);
}

std::shared_ptr<awst::Expression> StorageMapper::biguintSlotToBtoi(
	std::shared_ptr<awst::Expression> const& _slotExpr,
	awst::SourceLocation const& _loc
)
{
	auto castToBytes = awst::makeReinterpretCast(_slotExpr, awst::WType::bytesType(), _loc);
	auto last8 = awst::makeExtractLastN(std::move(castToBytes), 8, _loc);
	return awst::makeBtoi(std::move(last8), _loc);
}

} // namespace puyasol::builder
