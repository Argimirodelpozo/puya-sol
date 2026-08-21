/// @file SignedOps.cpp
/// Signed arithmetic: sdiv, smod, slt, sgt, sar, tload, tstore, isNegative256, negate256.

#include "builder/assembly/AssemblyBuilder.h"
#include "builder/storage/StorageLayout.h"
#include "builder/storage/StorageMapper.h" // makeStateGetWithDefault (box-keyed struct sstore)
#include "builder/sol-types/TypeCoercion.h" // isNegativeSigned (shared sign-bit test)
#include "Logger.h"
#include "builder/proxies/Erc1967Lowering.h"
#include "builder/BuildArtifacts.h"

#include <optional>
#include <sstream>

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
	awst::ARC4Struct const& _struct,
	std::string const& _operation,
	awst::SourceLocation const& _loc)
{
	std::vector<BoxStructSlotField> fields;
	fields.reserve(_struct.fields().size());
	int arc4Bit = 0;
	int evmSlot = 0;
	int evmBit = 0;
	for (auto const& [name, fieldType]: _struct.fields())
	{
		(void)name;
		bool const isBool = fieldType == awst::WType::arc4BoolType();
		int fieldBits = 0;
		if (isBool)
			fieldBits = 8; // Solidity storage gives bool a complete byte lane.
		else if (auto const* uintN = dynamic_cast<awst::ARC4UIntN const*>(fieldType);
			uintN && (uintN->n() % 8) == 0)
			fieldBits = uintN->n();
		else
		{
			Logger::instance().error(
				_operation + " of a box-keyed struct slot supports fixed-width "
				"integer and bool fields; field type '"
				+ std::string(fieldType ? fieldType->name() : "<null>")
				+ "' has no scalar EVM/ARC-4 slot mapping", _loc);
			return std::nullopt;
		}

		if (evmBit + fieldBits > 256)
		{
			++evmSlot;
			evmBit = 0;
		}

		// An ARC-4 bool run occupies consecutive bits. The next non-bool
		// starts after the run's final (possibly partial) byte.
		if (!isBool && (arc4Bit % 8) != 0)
			arc4Bit = ((arc4Bit + 7) / 8) * 8;
		fields.push_back({arc4Bit, evmSlot, evmBit, fieldBits, isBool});
		arc4Bit += isBool ? 1 : fieldBits;
		evmBit += fieldBits;
		if (evmBit == 256)
		{
			++evmSlot;
			evmBit = 0;
		}
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
	// extract3(TRANSIENT_SLOT blob, slot*32, 32) → biguint.
	// Scratch slot bzero'd in preamble; persists across callsub within an app call.
	if (_args.empty()) return nullptr;

	// The transient scratch slot is a fixed 4096-byte blob = 128 slots. A
	// keccak-derived slot (a transient MAPPING key) is a 32-byte biguint that
	// overflows btoi, and any slot >= 128 overruns the blob — both would panic
	// opaquely. Assert slot < 128 (fail loud on the unsupported cases), then
	// safeBtoi handles the now-bounded value.
	auto slot = awst::makeEvalOnce(ensureBiguint(_args[0], _loc), _loc);
	emitTransientSlotBound(slot, _loc, m_pendingStatements);
	auto slotU64 = safeBtoi(slot, _loc);
	auto offset = awst::makeUInt64BinOp(std::move(slotU64), awst::UInt64BinaryOperator::Mult,
		awst::makeIntegerConstant("32", _loc), _loc);

	return awst::makeAsBiguint(
		awst::makeExtract3(awst::makeLoadSlot(TRANSIENT_SLOT, _loc),
			std::move(offset), awst::makeIntegerConstant("32", _loc), _loc), _loc);
}

void AssemblyBuilder::handleTstore(
	std::vector<std::shared_ptr<awst::Expression>> const& _args,
	awst::SourceLocation const& _loc,
	std::vector<std::shared_ptr<awst::Statement>>& _out
)
{
	// replace3(TRANSIENT_SLOT blob, slot*32, zeroExtend(value, 32)).
	if (_args.size() < 2) return;

	auto slot = awst::makeEvalOnce(ensureBiguint(_args[0], _loc), _loc);
	emitTransientSlotBound(slot, _loc, _out);
	auto value = ensureBiguint(_args[1], _loc);
	auto slotU64 = safeBtoi(slot, _loc);
	auto offset = awst::makeUInt64BinOp(std::move(slotU64), awst::UInt64BinaryOperator::Mult,
		awst::makeIntegerConstant("32", _loc), _loc);
	auto padded = awst::makeZeroExtendToN(awst::makeAsBytes(std::move(value), _loc), 32, _loc);
	auto replace = awst::makeReplace3(awst::makeLoadSlot(TRANSIENT_SLOT, _loc),
		std::move(offset), std::move(padded), _loc);
	// Direct scratch write: side-effectful, can't be DCE'd, persists across callsub.
	_out.push_back(awst::makeExpressionStatement(
		awst::makeStoreSlot(TRANSIENT_SLOT, std::move(replace), _loc), _loc));
}

// ─── Signed integer helpers ──────────────────────────────────────────────────

std::shared_ptr<awst::Expression> AssemblyBuilder::isNegative256(
	std::shared_ptr<awst::Expression> _val,
	awst::SourceLocation const& _loc,
	awst::WType const* _origType
)
{
	// value ≥ 2^255 (biguint) or ≥ 2^63 (uint64) indicates a negative two's-complement.
	unsigned bits = (_origType && _origType == awst::WType::uint64Type()) ? 64 : 256;
	return TypeCoercion::isNegativeSigned(std::move(_val), bits, _loc);
}

std::shared_ptr<awst::Expression> AssemblyBuilder::negate256(
	std::shared_ptr<awst::Expression> _val,
	awst::SourceLocation const& _loc
)
{
	// Two's complement negate: MAX_UINT256 - val + 1  (= ~val + 1, mod 2^256).
	auto maxU256 = awst::makeIntegerConstant(
		"115792089237316195423570985008687907853269984665640564039457584007913129639935", // 2^256 - 1
		_loc, awst::WType::biguintType());

	auto sub = makeBigUIntBinOp(maxU256, awst::BigUIntBinaryOperator::Sub, _val, _loc);

	auto one = awst::makeBiguintConstant("1", _loc);

	// Wrap mod 2^256: negate256(0) = (2^256-1)+1 = 2^256 must reduce to 0 (two's
	// complement of 0 is 0), else the out-of-range 2^256 reverts. This is hit
	// whenever a signed asm op (sdiv/smod) produces a zero result with a negative
	// sign, e.g. sdiv(x, int256.min) or smod(int256.min, y) — EVM returns 0.
	// No-op for every val>=1 (2^256-val is already in range).
	auto sum = makeBigUIntBinOp(sub, awst::BigUIntBinaryOperator::Add, one, _loc);
	return wrapMod256(std::move(sum), _loc);
}

std::shared_ptr<awst::Expression> AssemblyBuilder::handleSdiv(
	std::vector<std::shared_ptr<awst::Expression>> const& _args,
	awst::SourceLocation const& _loc
)
{
	// sdiv(a, b): signed division; result sign = sign(a) XOR sign(b). sdiv(a,0)=0.
	if (!checkArity(_args, 2, "sdiv", _loc))
		return nullptr;

	auto a = ensureBiguint(_args[0], _loc);
	auto b = ensureBiguint(_args[1], _loc);

	auto absA = awst::makeConditional(
		isNegative256(a, _loc), negate256(a, _loc), a, awst::WType::biguintType(), _loc);
	auto absB = awst::makeConditional(
		isNegative256(b, _loc), negate256(b, _loc), b, awst::WType::biguintType(), _loc);
	auto quotient = makeBigUIntBinOp(absA, awst::BigUIntBinaryOperator::FloorDiv, absB, _loc);

	// resultNeg = aNeg XOR bNeg
	auto xorResult = awst::makeNumericCompare(
		ensureBiguint(isNegative256(a, _loc), _loc), awst::NumericComparison::Ne,
		ensureBiguint(isNegative256(b, _loc), _loc), _loc);
	auto signedResult = awst::makeConditional(
		std::move(xorResult), negate256(quotient, _loc), quotient,
		awst::WType::biguintType(), _loc);

	// b==0 guard: AVM b/ panics; the conditional only evaluates the taken branch.
	auto bNonZero = awst::makeNumericCompare(
		b, awst::NumericComparison::Ne, awst::makeBiguintConstant("0", _loc), _loc);
	return awst::makeConditional(
		std::move(bNonZero), std::move(signedResult),
		awst::makeBiguintConstant("0", _loc), awst::WType::biguintType(), _loc);
}

std::shared_ptr<awst::Expression> AssemblyBuilder::handleSmod(
	std::vector<std::shared_ptr<awst::Expression>> const& _args,
	awst::SourceLocation const& _loc
)
{
	// smod(a, b): sign of result = sign of a. smod(a,0)=0.
	if (!checkArity(_args, 2, "smod", _loc))
		return nullptr;

	auto a = ensureBiguint(_args[0], _loc);
	auto b = ensureBiguint(_args[1], _loc);

	auto absA = awst::makeConditional(
		isNegative256(a, _loc), negate256(a, _loc), a, awst::WType::biguintType(), _loc);
	auto absB = awst::makeConditional(
		isNegative256(b, _loc), negate256(b, _loc), b, awst::WType::biguintType(), _loc);
	auto remainder = makeBigUIntBinOp(absA, awst::BigUIntBinaryOperator::Mod, absB, _loc);
	auto signedResult = awst::makeConditional(
		isNegative256(a, _loc), negate256(remainder, _loc), remainder,
		awst::WType::biguintType(), _loc);

	// b==0 guard: AVM b% panics.
	auto bNonZero = awst::makeNumericCompare(
		b, awst::NumericComparison::Ne, awst::makeBiguintConstant("0", _loc), _loc);
	return awst::makeConditional(
		std::move(bNonZero), std::move(signedResult),
		awst::makeBiguintConstant("0", _loc), awst::WType::biguintType(), _loc);
}

std::shared_ptr<awst::Expression> AssemblyBuilder::handleSlt(
	std::vector<std::shared_ptr<awst::Expression>> const& _args,
	awst::SourceLocation const& _loc
)
{
	// slt(a,b): signed less-than (two's complement).
	if (!checkArity(_args, 2, "slt", _loc))
		return nullptr;

	// Capture original types before ensureBiguint: sign-bit threshold differs
	// (bit 63 for uint64, bit 255 for biguint).
	auto const* origTypeA = _args[0]->wtype;
	auto const* origTypeB = _args[1]->wtype;
	auto a = ensureBiguint(_args[0], _loc);
	auto b = ensureBiguint(_args[1], _loc);

	// slt(x,0) = isNegative(x): avoids puya constant-folding `a<0` to false.
	if (auto* bConst = dynamic_cast<awst::IntegerConstant*>(b.get()))
	{
		if (bConst->value == "0")
		{
			return ensureBiguint(isNegative256(a, _loc, origTypeA), _loc);
		}
	}

	// slt(0,x) = x>0 && x<2^(N-1) (positive non-zero).
	if (auto* aConst = dynamic_cast<awst::IntegerConstant*>(a.get()))
	{
		if (aConst->value == "0")
		{
			auto signThreshold = awst::makeIntegerConstant(
				origTypeB && origTypeB == awst::WType::uint64Type()
					? "9223372036854775808" // 2^63
					: "57896044618658097711785492504343953926634992332820282019728792003956564819968", // 2^255
				_loc, awst::WType::biguintType());
			auto andExpr = awst::makeBoolBinOp(
				awst::makeNumericCompare(b, awst::NumericComparison::Gt,
					awst::makeBiguintConstant("0", _loc), _loc),
				awst::BinaryBooleanOperator::And,
				awst::makeNumericCompare(b, awst::NumericComparison::Lt,
					std::move(signThreshold), _loc), _loc);
			return ensureBiguint(andExpr, _loc);
		}
	}

	// General: signsMatch ? (a < b) : aNeg.
	auto aNeg = isNegative256(a, _loc, origTypeA);
	auto aNeg2 = isNegative256(a, _loc, origTypeA);
	auto signsMatch = awst::makeNumericCompare(
		ensureBiguint(aNeg, _loc), awst::NumericComparison::Eq,
		ensureBiguint(isNegative256(b, _loc, origTypeB), _loc), _loc);
	auto result = awst::makeConditional(
		signsMatch,
		awst::makeNumericCompare(a, awst::NumericComparison::Lt, b, _loc),
		aNeg2, awst::WType::boolType(), _loc);
	return ensureBiguint(result, _loc);
}

std::shared_ptr<awst::Expression> AssemblyBuilder::handleSgt(
	std::vector<std::shared_ptr<awst::Expression>> const& _args,
	awst::SourceLocation const& _loc
)
{
	// sgt(a,b) = slt(b,a).
	if (!checkArity(_args, 2, "sgt", _loc))
		return nullptr;
	std::vector<std::shared_ptr<awst::Expression>> swapped = {_args[1], _args[0]};
	return handleSlt(swapped, _loc);
}

std::shared_ptr<awst::Expression> AssemblyBuilder::handleSar(
	std::vector<std::shared_ptr<awst::Expression>> const& _args,
	awst::SourceLocation const& _loc
)
{
	// sar(shift, value): arithmetic right shift.
	// Positive: result = shr(shift, value).
	// Negative: result = shr | fillMask (top `shift` bits set).
	// fillMask = MAX_UINT256 - 2^(256-shift) + 1.
	// sar(n>=256, x<0) = all-ones: complementShift clamped to 0 →
	// pow2Complement=1 → fillMask=MAX, so 0|MAX=MAX. AVM `-` panics on
	// underflow — the conditional only evaluates (256-shift) when shift<256.
	if (!checkArity(_args, 2, "sar", _loc))
		return nullptr;

	auto val = awst::makeEvalOnce(
		wrapMod256(ensureBiguint(_args[1], _loc), _loc), _loc);
	std::vector<std::shared_ptr<awst::Expression>> coercedArgs = {_args[0], val};
	auto shrResult = handleShr(coercedArgs, _loc);
	auto valNeg = isNegative256(val, _loc);

	// fillMask = the high `shift` sign bits = MAX - shr(shift, MAX). Using shr (which already
	// saturates to 0 for shift>=256 and is identity for shift==0) avoids the 2^256 / underflow edge
	// cases of the old 256-shift complement: at shift==0, shr(0,MAX)=MAX so fillMask=0 (sar(0,x)=x,
	// previously all-ones for negative x); at shift>=256, shr=0 so fillMask=MAX (all sign bits).
	auto maxStr = std::string(
		"115792089237316195423570985008687907853269984665640564039457584007913129639935");
	std::vector<std::shared_ptr<awst::Expression>> shrMaxArgs = {
		_args[0], awst::makeBiguintConstant(maxStr, _loc)};
	auto shrMax = handleShr(shrMaxArgs, _loc);
	auto fillMask = makeBigUIntBinOp(
		awst::makeBiguintConstant(maxStr, _loc), awst::BigUIntBinaryOperator::Sub,
		std::move(shrMax), _loc);

	auto negResult = awst::makeAsBiguint(
		awst::makeBytesOr(awst::makeAsBytes(shrResult, _loc),
			awst::makeAsBytes(fillMask, _loc), _loc), _loc);

	return awst::makeConditional(
		valNeg, negResult, handleShr(coercedArgs, _loc), awst::WType::biguintType(), _loc);
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
	switch (proxies::Erc1967Lowering::classify(_args[0].get()))
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
			proxies::Erc1967Slot::Implementation, /*_isStore=*/true, _loc));
		return;
	case proxies::Erc1967Slot::Beacon:
		Logger::instance().warning(
			"ERC-1967 beacon slot write lowers to a runtime failure (see "
			"proxy.md)", _loc);
		_out.push_back(proxies::Erc1967Lowering::trapStatement(
			proxies::Erc1967Slot::Beacon, /*_isStore=*/true, _loc));
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

	auto maybeFields = boxStructSlotLayout(*st, "sstore", _loc);
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

	auto maybeFields = boxStructSlotLayout(*st, "sload", _loc);
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


} // namespace puyasol::builder
