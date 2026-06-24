/// @file SignedOps.cpp
/// Signed arithmetic: sdiv, smod, slt, sgt, sar, tload, tstore, isNegative256, negate256.

#include "builder/assembly/AssemblyBuilder.h"
#include "builder/storage/StorageLayout.h"
#include "builder/storage/StorageMapper.h" // makeStateGetWithDefault (box-keyed struct sstore)
#include "builder/sol-types/TypeCoercion.h" // isNegativeSigned (shared sign-bit test)
#include "Logger.h"

#include <sstream>

namespace puyasol::builder
{

std::shared_ptr<awst::Expression> AssemblyBuilder::handleTload(
	std::vector<std::shared_ptr<awst::Expression>> const& _args,
	awst::SourceLocation const& _loc
)
{
	// extract3(TRANSIENT_SLOT blob, slot*32, 32) → biguint.
	// Scratch slot bzero'd in preamble; persists across callsub within an app call.
	if (_args.empty()) return nullptr;

	auto slot = ensureBiguint(_args[0], _loc);
	auto slotU64 = awst::makeBtoi(awst::makeAsBytes(std::move(slot), _loc), _loc);
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

	auto slot = ensureBiguint(_args[0], _loc);
	auto value = ensureBiguint(_args[1], _loc);
	auto slotU64 = awst::makeBtoi(awst::makeAsBytes(std::move(slot), _loc), _loc);
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

	return makeBigUIntBinOp(sub, awst::BigUIntBinaryOperator::Add, one, _loc);
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

	auto val = ensureBiguint(_args[1], _loc);
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

	// sstore(info.slot, packedWord): box-keyed ARC4 struct sentinel (V4 Pool.updateTick).
	// EVM packs fields into a 256-bit slot; rebuild the box bytes field-by-field
	// (box holds ARC4 layout, not EVM slot layout).
	if (auto box = std::dynamic_pointer_cast<awst::BoxValueExpression>(_args[0]))
		if (dynamic_cast<awst::ARC4Struct const*>(box->wtype))
		{
			handleBoxKeyedStructSlotStore(box, _args[1], _loc, _out);
			return;
		}

	auto slotArg = _args[0];
	if (slotArg->wtype == awst::WType::biguintType())
		slotArg = safeBtoi(std::move(slotArg), _loc);
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

	// Compute ARC4 byte offset/width and EVM bit offset per field.
	// Only byte-aligned ARC4UIntN fields supported (the only shape sliceable losslessly).
	struct FieldInfo { int arc4Off; int byteW; int evmSlot; int evmBit; };
	std::vector<FieldInfo> fields;
	int arc4Off = 0, curSlot = 0, curBit = 0;
	for (auto const& [fname, fwt]: st->fields())
	{
		(void)fname;
		auto const* uintN = dynamic_cast<awst::ARC4UIntN const*>(fwt);
		if (!uintN || (uintN->n() % 8) != 0)
		{
			Logger::instance().error(
				"sstore to a box-keyed struct slot requires byte-aligned integer "
				"fields", _loc);
			return;
		}
		int const bits = uintN->n();
		if (curBit + bits > 256) { curSlot++; curBit = 0; }
		fields.push_back({arc4Off, bits / 8, curSlot, curBit});
		curBit += bits;
		if (curBit == 256) { curSlot++; curBit = 0; }
		arc4Off += bits / 8;
	}

	// Bare `info.slot` addresses slot 0; add(info.slot,k) arrives as a binop and never reaches here.
	int const targetSlot = 0;

	// Packed word as 32 big-endian bytes.
	auto packedBytes = awst::makeLeftPadToN(
		awst::makeAsBytes(ensureBiguint(_packed, _loc), _loc), 32, _loc);

	// Existing box value as bytes (zero struct when the box is absent).
	auto existing = awst::makeAsBytes(
		builder::StorageMapper::makeStateGetWithDefault(
			awst::makeBoxValueExpression(_slotBox->key, _slotBox->wtype, _loc),
			_slotBox->wtype, _loc),
		_loc);

	// Rebuild struct bytes: written-slot fields from packedBytes (by EVM byte range),
	// all others from existing box (by ARC4 byte range).
	std::shared_ptr<awst::Expression> rebuilt;
	for (auto const& fi: fields)
	{
		std::shared_ptr<awst::Expression> chunk;
		if (fi.evmSlot == targetSlot)
		{
			// Big-endian byte range within 32-byte slot: [32-(bit+width)/8, 32-bit/8).
			int const byteOffInSlot = (256 - fi.evmBit - fi.byteW * 8) / 8;
			chunk = awst::makeExtract3(
				packedBytes,
				awst::makeIntegerConstant(static_cast<uint64_t>(byteOffInSlot), _loc),
				awst::makeIntegerConstant(static_cast<uint64_t>(fi.byteW), _loc),
				_loc);
		}
		else
			chunk = awst::makeExtract3(
				existing,
				awst::makeIntegerConstant(static_cast<uint64_t>(fi.arc4Off), _loc),
				awst::makeIntegerConstant(static_cast<uint64_t>(fi.byteW), _loc),
				_loc);

		rebuilt = rebuilt
			? awst::makeConcat(std::move(rebuilt), std::move(chunk), _loc)
			: std::move(chunk);
	}
	if (!rebuilt) return;

	auto target = awst::makeBoxValueExpression(_slotBox->key, _slotBox->wtype, _loc);
	auto newVal = awst::makeReinterpretCast(std::move(rebuilt), _slotBox->wtype, _loc);
	auto assign = awst::makeAssignmentExpression(std::move(target), std::move(newVal), _loc);
	_out.push_back(awst::makeExpressionStatement(std::move(assign), _loc));
}


} // namespace puyasol::builder
