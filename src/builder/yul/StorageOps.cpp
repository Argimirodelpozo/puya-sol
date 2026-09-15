/// @file StorageOps.cpp
/// EVM storage opcodes: sload, sstore, tload, tstore, and the box-keyed struct
/// slot accessors.
///
/// These lived in SignedOps.cpp and BitwiseShiftOps.cpp -- filed by the leading
/// "s", not by what they do. sload and sstore were in DIFFERENT files, and half
/// of SignedOps.cpp was storage rather than signed arithmetic.

#include "builder/yul/AssemblyBuilder.h"
#include "builder/storage/TransientStorage.h"
#include "builder/storage/StorageLayout.h"
#include "builder/storage/StorageMapper.h"
#include "builder/storage/slot/SlotHandleAccess.h"
#include "builder/types/TypeCoercion.h"
#include "builder/codec/Arc4Defaults.h"
#include "builder/lowering/proxies/Erc1967Lowering.h"
#include "builder/context/BuildArtifacts.h"
#include "builder/target/EvmFeaturePolicy.h"
#include "awst/NameGen.h"
#include "Logger.h"

#include <optional>
#include <sstream>
// yul nodes BY VALUE (the AST aliases are std::variant, which needs
// complete types).
#include <libyul/AST.h>
#include <libyul/Dialect.h>
#include <libsolidity/ast/AST.h>
#include <libsolidity/ast/Types.h>

namespace puyasol::builder
{

namespace
{

/// One scalar field's location in both representations of a box-backed
/// struct. Solidity allocates bool as an ordinary one-byte storage lane,
/// whereas ARC-4 packs each consecutive bool run MSB-first into as few bytes
/// as possible. Integer fields are byte-aligned in both representations.
struct BoxStructSlotField
{
	int arc4Bit = 0;
	int evmSlot = 0;
	int evmBit = 0;
	int evmBits = 0;
	bool isBool = false;
};

std::optional<std::vector<BoxStructSlotField>> boxStructSlotLayout(
	TypeMapper& types, awst::ARC4Struct const& _struct,
	std::string const& _operation, awst::SourceLocation const& _loc)
{
	auto const* source = dynamic_cast<solidity::frontend::StructType const*>(
		types.solcAggregateFor(&_struct));
	auto offsets = arc4FieldBitOffsets(_struct);
	if (!source || !offsets)
	{
		Logger::instance().error(_operation + " requires a fixed box struct with solc storage facts", _loc);
		return std::nullopt;
	}
	std::vector<BoxStructSlotField> fields;
	auto positions = SlotHandleAccess::fieldPositions(source, &_struct);
	for (size_t i = 0; i < positions.size(); ++i)
	{
		auto const& position = positions[i];
		auto const* member = position.solType;
		bool isBool = position.wtype == awst::WType::arc4BoolType();
		auto const* integer = dynamic_cast<awst::ARC4UIntN const*>(position.wtype);
		if (!member || !member->isValueType()
			|| (!isBool && (!integer || integer->n() != member->storageBytes() * 8)))
		{
			Logger::instance().error(_operation
				+ " of a box struct supports only matching fixed-width integer and bool fields", _loc);
			return std::nullopt;
		}
		fields.push_back({checkedSize<int>((*offsets)[i], "ARC4 field bit offset"),
			checkedSize<int>(position.slot, "struct member slot"), checkedSize<int>(position.byteOffset * 8, "storage bit offset"),
			checkedSize<int>(member->storageBytes() * 8, "storage field width"), isBool});
	}
	return fields;
}

int evmByteOffset(BoxStructSlotField const& _field)
{
	return (256 - _field.evmBit - _field.evmBits) / 8;
}

} // namespace


// Assert a transient slot fits the 4096-byte / 128-slot scratch blob. A
// keccak-derived mapping slot or a slot >= 128 would otherwise panic opaquely
// (btoi overflow / extract3 OOB). `_slot` must be an eval-once biguint.
static void emitTransientSlotBound(
	std::shared_ptr<awst::Expression> const& _slot,
	awst::SourceLocation const& _loc,
	std::vector<std::shared_ptr<awst::Statement>>& _out)
{
	auto ok = awst::makeNumericCompare(_slot, awst::NumericComparison::Lt,
		awst::makeIntegerConstant("128", _loc, awst::WType::biguintType()), _loc);
	_out.push_back(awst::makeExpressionStatement(
		awst::makeAssert(std::move(ok), _loc,
			"transient storage slot out of range (only slots 0..127 are "
			"supported on AVM; transient mappings are not)"), _loc));
}


std::shared_ptr<awst::Expression> AssemblyBuilder::handleTload(
	std::vector<std::shared_ptr<awst::Expression>> const& _args,
	awst::SourceLocation const& _loc
)
{
	// extract3(transient blob, slot*32, 32) → biguint.
	// Scratch slot bzero'd in preamble; persists across callsub within an app call.
	if (_args.empty()) return nullptr;

	// The transient scratch slot is a fixed 4096-byte blob = 128 slots. A
	// keccak-derived slot (a transient MAPPING key) is a 32-byte biguint that
	// overflows btoi, and any slot >= 128 overruns the blob — both would panic
	// opaquely. Assert slot < 128 (fail loud on the unsupported cases), then
	// safeBtoi handles the now-bounded value.
	auto slot = awst::makeEvalOnce(ensureBiguint(_args[0], _loc), _loc);
	emitTransientSlotBound(slot, _loc, m_frame.pendingStatements);
	auto slotU64 = safeBtoi(slot, _loc);
	auto offset = awst::makeUInt64BinOp(std::move(slotU64), awst::UInt64BinaryOperator::Mult,
		awst::makeIntegerConstant("32", _loc), _loc);

	return awst::makeAsBiguint(
		awst::makeExtract3(awst::makeLoadSlot(transientSlot(), _loc),
			std::move(offset), awst::makeIntegerConstant("32", _loc), _loc), _loc);
}


void AssemblyBuilder::handleTstore(
	std::vector<std::shared_ptr<awst::Expression>> const& _args,
	awst::SourceLocation const& _loc,
	std::vector<std::shared_ptr<awst::Statement>>& _out
)
{
	// replace3(transient blob, slot*32, zeroExtend(value, 32)).
	if (_args.size() < 2) return;

	auto slot = awst::makeVarExpression("__tstore_slot_" + std::to_string(
		awst::NameGen::next("AssemblyBuilder.tstoreSlot")), awst::WType::biguintType(), _loc);
	_out.push_back(awst::makeAssignmentStatement(slot, ensureBiguint(_args[0], _loc), _loc));
	emitTransientSlotBound(slot, _loc, _out);
	auto value = ensureBiguint(_args[1], _loc);
	auto slotU64 = safeBtoi(slot, _loc);
	auto offset = awst::makeUInt64BinOp(std::move(slotU64), awst::UInt64BinaryOperator::Mult,
		awst::makeIntegerConstant("32", _loc), _loc);
	auto padded = awst::makeZeroExtendToN(awst::makeAsBytes(std::move(value), _loc), 32, _loc);
	auto replace = awst::makeReplace3(awst::makeLoadSlot(transientSlot(), _loc),
		std::move(offset), std::move(padded), _loc);
	// Direct scratch write: side-effectful, can't be DCE'd, persists across callsub.
	_out.push_back(awst::makeExpressionStatement(
		awst::makeStoreSlot(transientSlot(), std::move(replace), _loc), _loc));
	if (m_context->transientStorage)
		m_context->transientStorage->clearAddressShadowForWord(slot, _out, _loc);
}


void AssemblyBuilder::handleSstore(
	std::vector<std::shared_ptr<awst::Expression>> const& _args,
	awst::SourceLocation const& _loc,
	std::vector<std::shared_ptr<awst::Statement>>& _out
)
{
	if (!checkArity(_args, 2, "sstore", _loc))
		return;

	// EIP-1967 proxy slots (proxy.md §1): admin writes land on the
	// synthesized global (arming the native-update gate); implementation/
	// beacon writes are runtime traps — upgradeTo lowers to the native
	// UpdateApplication ceremony, and unreachable sites strip via DCE
	// (the delegatecall precedent).
	switch (m_typeMapper.profile().proxyAdaptation
		? proxies::Erc1967Lowering::classify(_args[0].get())
		: proxies::Erc1967Slot::None)
	{
	case proxies::Erc1967Slot::Admin:
		m_typeMapper.artifacts().noteErc1967AdminUse();
		proxies::Erc1967Lowering::adminStore(
			ensureBiguint(_args[1], _loc), _loc, _out);
		return;
	case proxies::Erc1967Slot::Implementation:
		Logger::instance().warning(
			"ERC-1967 implementation-slot write (upgradeTo) lowers to a runtime "
			"failure — the AVM upgrade is a native UpdateApplication submitted "
			"by the admin with the new program (see proxy.md)", _loc);
		_out.push_back(proxies::Erc1967Lowering::trapStatement(
			proxies::Erc1967Slot::Implementation, _loc));
		return;
	case proxies::Erc1967Slot::Beacon:
		Logger::instance().warning(
			"ERC-1967 beacon slot write lowers to a runtime failure (see "
			"proxy.md)", _loc);
		_out.push_back(proxies::Erc1967Lowering::trapStatement(
			proxies::Erc1967Slot::Beacon, _loc));
		return;
	case proxies::Erc1967Slot::None:
		break;
	}

	// sstore(info.slot, packedWord): box-keyed ARC4 struct sentinel (V4 Pool.updateTick).
	// EVM packs fields into a 256-bit slot; rebuild the box bytes field-by-field
	// (box holds ARC4 layout, not EVM slot layout).
	if (auto box = std::dynamic_pointer_cast<awst::BoxValueExpression>(_args[0]))
		if (dynamic_cast<awst::ARC4Struct const*>(box->wtype))
		{
			handleBoxKeyedStructSlotStore(box, _args[1], _loc, _out);
			return;
		}

	// CONSTANT slot → route directly to the named variable's storage.
	if (tryRouteConstSlotStore(_args[0], _args[1], _loc, _out))
		return;

	// Full-width slot: __storage_write takes the 256-bit slot (no truncation).
	auto slotArg = ensureBiguintSlotArg(_args[0], _loc);
	auto valueArg = ensureBiguint(_args[1], _loc);

	auto call = awst::makeSubroutineCall(awst::SubroutineID{"__puyasol___storage_write"}, awst::WType::voidType(), _loc);

	awst::pushCallArg(call->args, "__slot", std::move(slotArg));
	awst::pushCallArg(call->args, "__value", std::move(valueArg));

	auto stmt = awst::makeExpressionStatement(std::move(call), _loc);
	_out.push_back(std::move(stmt));
}


void AssemblyBuilder::handleBoxKeyedStructSlotStore(
	std::shared_ptr<awst::BoxValueExpression> const& _slotBox,
	std::shared_ptr<awst::Expression> const& _packed,
	awst::SourceLocation const& _loc,
	std::vector<std::shared_ptr<awst::Statement>>& _out
)
{
	auto const* st = dynamic_cast<awst::ARC4Struct const*>(_slotBox->wtype);
	if (!st) return; // guaranteed by caller; defensive

	auto maybeFields = boxStructSlotLayout(m_typeMapper, *st, "sstore", _loc);
	if (!maybeFields) return;
	auto const& fields = *maybeFields;

	// Bare `info.slot` addresses slot 0; add(info.slot,k) arrives as a binop and never reaches here.
	int const targetSlot = 0;

	// Packed word as 32 big-endian bytes.
	auto packedBytes = awst::makeEvalOnce(awst::makeLeftPadToN(
		awst::makeAsBytes(ensureBiguint(_packed, _loc), _loc), 32, _loc), _loc);

	// Single 32-byte field (solady Uint8Set/Heap): the box content IS the slot
	// word. Low-level box_put (create-or-replace) — this works for a RUNTIME box
	// key from a storage-ref param, where the high-level box-value assignment
	// below needs a static box declaration and otherwise asserts in puya.
	if (fields.size() == 1 && !fields[0].isBool
		&& fields[0].evmBits == 256 && fields[0].evmSlot == 0)
	{
		_out.push_back(awst::makeExpressionStatement(
			awst::makeBoxPut(_slotBox->key, std::move(packedBytes), _loc), _loc));
		return;
	}

	// Existing box value as bytes (zero struct when the box is absent).
	auto existing = awst::makeAsBytes(
		builder::StorageMapper::makeStateGetWithDefault(
			awst::makeBoxValueExpression(_slotBox->key, _slotBox->wtype, _loc),
			_slotBox->wtype, _loc),
		_loc);

	// Overlay written-slot fields onto the existing ARC-4 bytes. Integer
	// fields replace their byte ranges. Bool fields copy the truth value from
	// their complete EVM byte lane into the appropriate ARC-4 packed bit.
	std::shared_ptr<awst::Expression> rebuilt = std::move(existing);
	for (auto const& fi: fields)
	{
		if (fi.evmSlot != targetSlot)
			continue;
		int const byteOffInSlot = evmByteOffset(fi);
		if (fi.isBool)
		{
			auto lane = awst::makeExtract3(
				packedBytes,
				awst::makeIntegerConstant(static_cast<uint64_t>(byteOffInSlot), _loc),
				awst::makeIntegerConstant("1", _loc),
				_loc);
			auto truth = awst::makeNumericCompare(
				awst::makeBtoi(std::move(lane), _loc), awst::NumericComparison::Ne,
				awst::makeZero(_loc), _loc);
			rebuilt = awst::makeSetbit(
				std::move(rebuilt),
				awst::makeIntegerConstant(static_cast<uint64_t>(fi.arc4Bit), _loc),
				std::move(truth), _loc);
		}
		else
		{
			auto chunk = awst::makeExtract3(
				packedBytes,
				awst::makeIntegerConstant(static_cast<uint64_t>(byteOffInSlot), _loc),
				awst::makeIntegerConstant(static_cast<uint64_t>(fi.evmBits / 8), _loc),
				_loc);
			rebuilt = awst::makeReplace3(
				std::move(rebuilt),
				awst::makeIntegerConstant(static_cast<uint64_t>(fi.arc4Bit / 8), _loc),
				std::move(chunk), _loc);
		}
	}

	auto target = awst::makeBoxValueExpression(_slotBox->key, _slotBox->wtype, _loc);
	auto newVal = awst::makeReinterpretCast(std::move(rebuilt), _slotBox->wtype, _loc);
	auto assign = awst::makeAssignmentExpression(std::move(target), std::move(newVal), _loc);
	_out.push_back(awst::makeExpressionStatement(std::move(assign), _loc));
}


std::shared_ptr<awst::Expression> AssemblyBuilder::handleBoxKeyedStructSlotLoad(
	std::shared_ptr<awst::BoxValueExpression> const& _slotBox,
	awst::SourceLocation const& _loc
)
{
	// Inverse of handleBoxKeyedStructSlotStore: `sload(s.slot)` reads the EVM
	// slot-0 packed word from the ARC4 box — each slot-0 field placed at its EVM
	// byte position within a 32-byte word. Single-uint256-field structs (solady
	// Uint8Set/Heap) → the box's 32 bytes ARE the word.
	auto const* st = dynamic_cast<awst::ARC4Struct const*>(_slotBox->wtype);
	if (!st) return nullptr;

	auto maybeFields = boxStructSlotLayout(m_typeMapper, *st, "sload", _loc);
	if (!maybeFields) return nullptr;
	auto const& fields = *maybeFields;

	int const targetSlot = 0; // bare `s.slot`; add(s.slot,k) arrives as a binop, not here
	auto existing = awst::makeEvalOnce(awst::makeAsBytes(
		builder::StorageMapper::makeStateGetWithDefault(
			awst::makeBoxValueExpression(_slotBox->key, _slotBox->wtype, _loc),
			_slotBox->wtype, _loc),
		_loc), _loc);

	std::shared_ptr<awst::Expression> word = awst::makeBzero(32, _loc);
	for (auto const& fi: fields)
	{
		if (fi.evmSlot != targetSlot) continue;
		int const byteOffInSlot = evmByteOffset(fi);
		if (fi.isBool)
		{
			auto truth = awst::makeGetbit(
				existing,
				awst::makeIntegerConstant(static_cast<uint64_t>(fi.arc4Bit), _loc),
				_loc);
			// setbit indexes bytes MSB-first. The canonical EVM bool lives in
			// the least-significant bit of its one-byte storage lane.
			word = awst::makeSetbit(
				std::move(word),
				awst::makeIntegerConstant(
					static_cast<uint64_t>(byteOffInSlot * 8 + 7), _loc),
				std::move(truth), _loc);
		}
		else
		{
			auto chunk = awst::makeExtract3(existing,
				awst::makeIntegerConstant(static_cast<uint64_t>(fi.arc4Bit / 8), _loc),
				awst::makeIntegerConstant(static_cast<uint64_t>(fi.evmBits / 8), _loc),
				_loc);
			word = awst::makeReplace3(std::move(word),
				awst::makeIntegerConstant(static_cast<uint64_t>(byteOffInSlot), _loc),
				std::move(chunk), _loc);
		}
	}
	return awst::makeAsBiguint(std::move(word), _loc);
}


std::shared_ptr<awst::Expression> AssemblyBuilder::handleSload(
	std::vector<std::shared_ptr<awst::Expression>> const& _args,
	awst::SourceLocation const& _loc
)
{
	if (!checkArity(_args, 1, "sload", _loc))
		return nullptr;

	// EIP-1967 proxy slots (proxy.md §1): admin → synthesized global,
	// implementation → this app's own identity, beacon → runtime trap.
	switch (m_typeMapper.profile().proxyAdaptation
		? proxies::Erc1967Lowering::classify(_args[0].get())
		: proxies::Erc1967Slot::None)
	{
	case proxies::Erc1967Slot::Admin:
		m_typeMapper.artifacts().noteErc1967AdminUse();
		return proxies::Erc1967Lowering::adminLoad(_loc);
	case proxies::Erc1967Slot::Implementation:
		return proxies::Erc1967Lowering::implementationLoad(_loc);
	case proxies::Erc1967Slot::Beacon:
		Logger::instance().warning(
			"ERC-1967 beacon slot read lowers to a runtime failure — this call "
			"site REVERTS if ever reached (see proxy.md)", _loc);
		m_frame.pendingStatements.push_back(proxies::Erc1967Lowering::trapStatement(
			proxies::Erc1967Slot::Beacon, _loc));
		return awst::makeBiguintConstant("0", _loc);
	case proxies::Erc1967Slot::None:
		break;
	}

	// CONSTANT slot → route directly to the named variable's storage (scalar
	// global / array length / array element). See SlotRoute.
	if (auto routed = tryRouteConstSlotLoad(_args[0], _loc))
		return routed;

	// Box-keyed ARC4 struct slot sentinel (`sload(s.slot)` where `s` is a struct
	// storage-ref param/alias): read the EVM slot-0 packed word from the box.
	if (auto box = std::dynamic_pointer_cast<awst::BoxValueExpression>(_args[0]))
		if (dynamic_cast<awst::ARC4Struct const*>(box->wtype))
			return handleBoxKeyedStructSlotLoad(box, _loc);

	// Full-width slot: __storage_read takes the 256-bit slot (no truncation).
	auto slotArg = ensureBiguintSlotArg(_args[0], _loc);

	auto call = awst::makeSubroutineCall(awst::SubroutineID{"__puyasol___storage_read"}, awst::WType::biguintType(), _loc);

	awst::pushCallArg(call->args, "__slot", std::move(slotArg));

	return call;
}

} // namespace puyasol::builder
