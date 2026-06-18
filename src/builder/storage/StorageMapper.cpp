#include "builder/storage/StorageMapper.h"
#include "builder/contract/StateVarWalker.h"
#include "builder/sol-types/Arc4Defaults.h"
#include "builder/sol-types/TypeCoercion.h"
#include "builder/sol-ast/StorageRefPointer.h"

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

std::shared_ptr<awst::Expression> StorageMapper::makeStateGetWithDefault(
	std::shared_ptr<awst::Expression> _field,
	awst::WType const* _type,
	awst::SourceLocation const& _loc
)
{
	// Skip StateGet wrapper for box reads that would breach AVM's 4 KB stack-value cap:
	//   (a) puyabug.md §4c: fixed-type bzero(N>4096) default reverts.
	//   (b) puyabug.md §4d: box_get returns the full box — reverts at runtime for
	//       boxes grown past 4 KB.
	// Bare BoxValueExpression is safe: puya lowers to BoxRead; add_box_extract_replace
	// folds into box_extract (no full-box load, no large default). BoxRead asserts
	// the box exists, so only eagerly-created boxes qualify:
	//   (a) Statically oversized fixed types — always eagerly box_created.
	//   (b) Top-level dynamic vars — eagerly created in __postInit.
	//       Mapping values (key = runtime concat/hash) are lazy; they stay on StateGet.
	if (auto bv = std::dynamic_pointer_cast<awst::BoxValueExpression>(_field))
	{
		// (a) Statically oversized fixed types — always eagerly created.
		if (builder::computeEncodedElementSize(_type) > kAvmStackValueMax)
			return _field;
		// (b) Top-level dynamic state vars — eagerly created in __postInit.
		if (isTopLevelDynamicBox(bv.get()))
			return _field;
	}
	auto def = makeDefaultValue(_type, _loc);
	return awst::makeStateGet(std::move(_field), std::move(def), _type, _loc);
}

int StorageMapper::computeEncodedElementSize(awst::WType const* _type)
{
	return builder::computeEncodedElementSize(_type);
}

std::shared_ptr<awst::BoxValueExpression> StorageMapper::makeTopLevelBoxExpr(
	std::string const& _varName,
	awst::WType const* _type,
	awst::SourceLocation const& _loc)
{
	auto key = awst::makeUtf8BytesConstant(_varName, _loc, awst::WType::boxKeyType());
	return awst::makeBoxValueExpression(std::move(key), _type, _loc);
}

bool StorageMapper::isTopLevelDynamicBox(awst::BoxValueExpression const* _box)
{
	if (!_box || !_box->wtype) return false;
	auto kind = _box->wtype->kind();
	bool dynamicSized =
		kind == awst::WTypeKind::ARC4DynamicArray
		|| kind == awst::WTypeKind::ReferenceArray
		|| (kind == awst::WTypeKind::Bytes
			&& !dynamic_cast<awst::BytesWType const*>(_box->wtype)->length().has_value());
	if (!dynamicSized) return false;
	// Top-level = key is a BytesConstant; mapping values use runtime concat/hash.
	return _box->key
		&& std::dynamic_pointer_cast<awst::BytesConstant>(_box->key) != nullptr;
}

bool StorageMapper::isMappingDerivedKey(awst::Expression const* _key)
{
	if (!_key) return false;
	if (dynamic_cast<awst::BoxPrefixedKeyExpression const*>(_key)) return true;
	if (auto const* ic = dynamic_cast<awst::IntrinsicCall const*>(_key))
		return ic->opCode == "sha256";
	return false;
}

std::shared_ptr<awst::Expression> StorageMapper::makeBoxLenTuple(
	TypeMapper& _typeMapper,
	std::shared_ptr<awst::Expression> _key,
	awst::SourceLocation const& _loc)
{
	auto* tupleType = _typeMapper.createType<awst::WTuple>(
		std::vector<awst::WType const*>{
			awst::WType::uint64Type(), awst::WType::boolType()});
	return awst::makeBoxLen(std::move(_key), tupleType, _loc);
}

// ── Multi-box helpers ──

// Multi-box: supports scalar elements (ARC4UIntN / fixed bytes) and nested
// ARC4StaticArray of scalars only. Struct elements need copy-on-write
// (box_extract → modify → box_replace); not implemented — fall back to warning.
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
		// Nested static array: recurse.
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

	// Mappings → box.
	if (type->category() == solidity::frontend::Type::Category::Mapping)
		return true;

	// Dynamic arrays/bytes → box. Strings stay in global state (typically short).
	if (auto const* arrType = dynamic_cast<solidity::frontend::ArrayType const*>(type))
	{
		if (arrType->isDynamicallySized() && !arrType->isString())
			return true;
		// Static outer array with dynamic element (e.g. uint[][2]): 2-slot
		// upper bound misleads — encoded payload can be arbitrary.
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

	// Structs passed by reference somewhere → box (handle-model Stage 1b): boxing makes the
	// ref a box-key handle that writes through into contract methods. Targeted to ref-passed
	// types (refPassedStructRegistry) so never-ref-passed structs keep their app-global layout.
	if (auto const* st = dynamic_cast<solidity::frontend::StructType const*>(type))
		if (puyasol::builder::refPassedStructRegistry().count(st->structDefinition().id()) > 0)
			return true;

	// AVM global-state limit is 128B (key+value). storageSizeUpperBound()*32 estimates value size.
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

	forEachStateVar(_contract, [&](auto const* var)
	{
		if (var->isConstant())
			return;
		if (var->referenceLocation() == solidity::frontend::VariableDeclaration::Location::Transient)
			return;
		if (seen.count(var->name()))
			return;
		seen.insert(var->name());

		awst::AppStorageDefinition def;
		def.sourceLocation = makeLoc(var->location(), _sourceFile);
		def.memberName = var->name();

		if (shouldUseBoxStorage(*var))
		{
			def.storageKind = awst::AppStorageKind::Box;

			// Unwrap nested mappings to find the final non-mapping value type.
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
	});

	return defs;
}

std::shared_ptr<awst::Expression> StorageMapper::makeStorageTarget(
	std::shared_ptr<awst::BytesConstant> const& _key,
	awst::WType const* _type,
	awst::AppStorageKind _kind,
	awst::SourceLocation const& _loc
)
{
	if (_kind == awst::AppStorageKind::Box)
		return awst::makeBoxValueExpression(_key, _type, _loc);
	// AppGlobal (Transient is dispatched by StorageBackend before reaching here).
	return awst::makeAppStateExpression(_key, _type, _loc);
}

std::shared_ptr<awst::Expression> StorageMapper::createStateRead(
	std::string const& _varName,
	awst::WType const* _type,
	awst::AppStorageKind _kind,
	awst::SourceLocation const& _loc
)
{
	auto key = makeKeyExpr(_varName, _loc, _kind);
	auto target = makeStorageTarget(key, _type, _kind, _loc);

	// Box: gates on 4 KB stack-value cap; see makeStateGetWithDefault.
	if (_kind == awst::AppStorageKind::Box)
		return makeStateGetWithDefault(std::move(target), _type, _loc);

	// AppGlobal: assert the var exists. Safe — every global is pre-written in
	// emitStateVarInit, so the assert never fires.
	if (auto app = std::dynamic_pointer_cast<awst::AppStateExpression>(target))
		app->existsAssertionMessage = "check " + _varName + " exists";
	return target;
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
	auto target = makeStorageTarget(key, _type, _kind, _loc);
	return awst::makeAssignmentExpression(std::move(target), std::move(_value), _loc, _type);
}

std::shared_ptr<awst::Expression> StorageMapper::biguintSlotToBtoi(
	std::shared_ptr<awst::Expression> const& _slotExpr,
	awst::SourceLocation const& _loc
)
{
	auto castToBytes = awst::makeAsBytes(_slotExpr, _loc);
	// biguint strips leading zeros, so a small slot (e.g. base+2) may encode to
	// <8 bytes — extractLastN(8) would underflow (AVM uint64 sub panics). Pad to
	// >=8 bytes first (b|(bzero(8),v) = max(len,8)) before the low-8 truncation.
	auto atLeast8 = awst::makeZeroExtendToN(std::move(castToBytes), 8, _loc);
	auto last8 = awst::makeExtractLastN(std::move(atLeast8), 8, _loc);
	return awst::makeBtoi(std::move(last8), _loc);
}

} // namespace puyasol::builder
