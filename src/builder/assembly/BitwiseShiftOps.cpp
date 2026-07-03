/// @file BitwiseShiftOps.cpp
/// Bitwise and shift operations: shl, shr, div, byte, signextend, buildPowerOf2.

#include "builder/assembly/AssemblyBuilder.h"
#include "builder/storage/StorageLayout.h"
#include "Logger.h"

#include <sstream>

namespace puyasol::builder
{

std::shared_ptr<awst::Expression> AssemblyBuilder::handleSload(
	std::vector<std::shared_ptr<awst::Expression>> const& _args,
	awst::SourceLocation const& _loc
)
{
	if (!checkArity(_args, 1, "sload", _loc))
		return nullptr;

	auto slotArg = _args[0];
	if (slotArg->wtype == awst::WType::biguintType())
		slotArg = safeBtoi(std::move(slotArg), _loc);

	auto call = awst::makeSubroutineCall(awst::SubroutineID{"__puyasol___storage_read"}, awst::WType::biguintType(), _loc);

	awst::pushCallArg(call->args, "__slot", std::move(slotArg));

	return call;
}

std::shared_ptr<awst::Expression> AssemblyBuilder::handleGas(
	awst::SourceLocation const& _loc
)
{
	// Returns uint64; consumer coerces via ensureBiguint (match at consumption, drops itob widen).
	Logger::instance().debug(
		"gas() mapped to AVM OpcodeBudget (analogous but not equivalent to EVM gas)", _loc);
	return awst::makeGlobal(std::string("OpcodeBudget"), awst::WType::uint64Type(), _loc);
}

std::shared_ptr<awst::Expression> AssemblyBuilder::handleTimestamp(
	awst::SourceLocation const& _loc
)
{
	// Returns uint64; consumer coerces (same natural-type convention as gas/number/selfbalance).
	return awst::makeGlobal("LatestTimestamp", awst::WType::uint64Type(), _loc);
}

std::shared_ptr<awst::Expression> AssemblyBuilder::buildPowerOf2(
	std::shared_ptr<awst::Expression> _shift,
	awst::SourceLocation const& _loc
)
{
	// 2^shift via setbit(bzero(32), 255-shift, 1) — AVM has no bexp opcode.
	auto shiftAmt = _shift;

	if (shiftAmt->wtype != awst::WType::uint64Type())
	{
		auto cast = awst::makeAsBytes(std::move(shiftAmt), _loc);
		auto cat = awst::makeLeftPad(std::move(cast), 8, _loc);
		auto extract = awst::makeExtractLastN(std::move(cat), 8, _loc);
		shiftAmt = awst::makeBtoi(std::move(extract), _loc);
	}

	auto bzero = awst::makeBzero(32, _loc);

	// Clamp to 0..255: puya optimizer may strip wrapMod256 from intermediates.
	auto clampedShift = awst::makeUInt64BinOp(std::move(shiftAmt), awst::UInt64BinaryOperator::Mod,
		awst::makeIntegerConstant("256", _loc), _loc);

	// setbit uses MSB-first: bit (255-shift) == 2^shift.
	auto bitIdx = awst::makeUInt64BinOp(
		awst::makeIntegerConstant("255", _loc),
		awst::UInt64BinaryOperator::Sub, std::move(clampedShift), _loc);

	return awst::makeAsBiguint(
		awst::makeSetbit(std::move(bzero), std::move(bitIdx), awst::makeOne(_loc), _loc), _loc);
}

std::shared_ptr<awst::Expression> AssemblyBuilder::handleDiv(
	std::vector<std::shared_ptr<awst::Expression>> const& _args,
	awst::SourceLocation const& _loc
)
{
	if (!checkArity(_args, 2, "div", _loc))
		return nullptr;
	// EVM div(a,0)=0; AVM panics.
	return safeDivMod(
		_args[0], awst::BigUIntBinaryOperator::FloorDiv, _args[1], _loc
	);
}

std::shared_ptr<awst::Expression> AssemblyBuilder::handleShl(
	std::vector<std::shared_ptr<awst::Expression>> const& _args,
	awst::SourceLocation const& _loc
)
{
	// shl(shift, value) → (value * 2^shift) mod 2^256, or 0 when shift ≥ 256 (EIP-145).
	// Arg order is (shift, value) — reversed vs. C/AVM.
	if (!checkArity(_args, 2, "shl", _loc))
		return nullptr;
	// Materialize once: shift feeds both buildPowerOf2 and the <256 conditional
	// (makeEvalOnce = OperandPlan primitive; skips SE on a constant amount).
	auto shift = awst::makeEvalOnce(ensureBiguint(_args[0], _loc), _loc);
	// Reduce value mod 2^256 before multiplying. Negative int128 decoded as
	// arc4.uint512 yields a 512-bit biguint; (512-bit * 2^s) overflows AVM's
	// 64-byte bigint limit before the trailing mod can reduce it (V4 Pool.updateTick
	// shl(128, liquidityNet)). Pre-reduction is a no-op for values already <2^256.
	auto value = wrapMod256(ensureBiguint(_args[1], _loc), _loc);
	auto power = buildPowerOf2(shift, _loc);
	auto product = makeBigUIntBinOp(
		value, awst::BigUIntBinaryOperator::Mult, std::move(power), _loc
	);
	auto wrapped = wrapMod256(std::move(product), _loc);
	auto twoFiftySix = awst::makeIntegerConstant(
		"256", _loc, awst::WType::biguintType());
	auto cond = awst::makeNumericCompare(
		shift, awst::NumericComparison::Lt, std::move(twoFiftySix), _loc);
	auto zero = awst::makeIntegerConstant(
		"0", _loc, awst::WType::biguintType());
	return awst::makeConditional(
		std::move(cond), std::move(wrapped), std::move(zero),
		awst::WType::biguintType(), _loc);
}

std::shared_ptr<awst::Expression> AssemblyBuilder::handleShr(
	std::vector<std::shared_ptr<awst::Expression>> const& _args,
	awst::SourceLocation const& _loc
)
{
	// shr(shift, value) → value / 2^shift, or 0 when shift ≥ 256 (EIP-145).
	// `shr(sub(256,…), MASK)` → shr(256,…) at bucket boundaries must return 0
	// (see AAVE PositionStatusMap.sol:223). AVM b>> has no such clamping.
	if (!checkArity(_args, 2, "shr", _loc))
		return nullptr;
	// Materialize once: shift feeds both buildPowerOf2 and the <256 conditional
	// (makeEvalOnce = OperandPlan primitive; skips SE on a constant amount).
	auto shift = awst::makeEvalOnce(ensureBiguint(_args[0], _loc), _loc);
	auto value = _args[1];
	auto power = buildPowerOf2(shift, _loc);
	auto divResult = makeBigUIntBinOp(
		value, awst::BigUIntBinaryOperator::FloorDiv, std::move(power), _loc
	);
	auto twoFiftySix = awst::makeIntegerConstant(
		"256", _loc, awst::WType::biguintType());
	auto cond = awst::makeNumericCompare(
		shift, awst::NumericComparison::Lt, std::move(twoFiftySix), _loc);
	auto zero = awst::makeIntegerConstant(
		"0", _loc, awst::WType::biguintType());
	return awst::makeConditional(
		std::move(cond), std::move(divResult), std::move(zero),
		awst::WType::biguintType(), _loc);
}

std::shared_ptr<awst::Expression> AssemblyBuilder::handleByte(
	std::vector<std::shared_ptr<awst::Expression>> const& _args,
	awst::SourceLocation const& _loc
)
{
	// byte(n, x): extract byte n from 32-byte big-endian x → biguint.
	if (!checkArity(_args, 2, "byte", _loc))
		return nullptr;

	auto padded = padTo32Bytes(_args[1], _loc);
	auto nExpr = _args[0];
	if (nExpr->wtype != awst::WType::uint64Type())
		nExpr = safeBtoi(std::move(nExpr), _loc);

	// n >= 32: EVM byte() returns 0 (out of range); the AVM extract3 at offset n would revert. Guard
	// with `n < 32 ? byte : 0` — the conditional only evaluates the extract on the taken branch. n is
	// used by both the bound check and the extract, so single-evaluate it.
	auto nSE = awst::makeEvalOnce(std::move(nExpr), _loc);
	auto inRange = awst::makeNumericCompare(
		nSE, awst::NumericComparison::Lt, awst::makeIntegerConstant("32", _loc), _loc);
	auto extracted = awst::makeAsBiguint(
		awst::makeExtract3(std::move(padded), nSE, awst::makeOne(_loc), _loc), _loc);
	return awst::makeConditional(
		std::move(inRange), std::move(extracted),
		awst::makeBiguintConstant("0", _loc), awst::WType::biguintType(), _loc);
}

std::shared_ptr<awst::Expression> AssemblyBuilder::handleSignextend(
	std::vector<std::shared_ptr<awst::Expression>> const& _args,
	awst::SourceLocation const& _loc
)
{
	// signextend(b, x): sign-extend x from byte b (0-indexed from low byte).
	// Identity: signextend(b,x) == sar(s, shl(s, x)), s = 248-8*min(b,31).
	// shl moves byte b's sign bit to bit 255; sar replicates it down.
	if (!checkArity(_args, 2, "signextend", _loc))
		return nullptr;

	auto x = ensureBiguint(_args[1], _loc);

	// STRICT literal check, NOT resolveConstantOffset: that helper recurses into
	// ABI-decode expressions and would fold a runtime `b` to the selector offset (4).
	std::optional<uint64_t> bConst;
	if (auto* lit = dynamic_cast<awst::IntegerConstant*>(_args[0].get()))
		bConst = std::stoull(lit->value);
	if (!bConst.has_value())
	{
		// Runtime b: s = 248 - 8*min(b,31). min(b,31) gives b≥31 no-op for free
		// (s=0 → sar(0,shl(0,x))=x) and prevents 248-8*b from underflowing.
		auto biguint = awst::WType::biguintType();
		auto buildS = [&]() -> std::shared_ptr<awst::Expression> {
			auto cmp = awst::makeNumericCompare(
				ensureBiguint(_args[0], _loc), awst::NumericComparison::Lt,
				awst::makeIntegerConstant("31", _loc, biguint), _loc);
			auto bc = awst::makeConditional(
				std::move(cmp), ensureBiguint(_args[0], _loc),
				awst::makeIntegerConstant("31", _loc, biguint), biguint, _loc);
			auto eightBc = makeBigUIntBinOp(
				std::move(bc), awst::BigUIntBinaryOperator::Mult,
				awst::makeIntegerConstant("8", _loc, biguint), _loc);
			return makeBigUIntBinOp(
				awst::makeIntegerConstant("248", _loc, biguint),
				awst::BigUIntBinaryOperator::Sub, std::move(eightBc), _loc);
		};
		std::vector<std::shared_ptr<awst::Expression>> shlArgs{
			buildS(), ensureBiguint(_args[1], _loc)};
		auto shifted = handleShl(shlArgs, _loc);
		std::vector<std::shared_ptr<awst::Expression>> sarArgs{
			buildS(), std::move(shifted)};
		return handleSar(sarArgs, _loc);
	}

	uint64_t b = *bConst;
	if (b >= 31)
		return x; // signextend from byte ≥31 is a no-op for 256-bit values

	// Same sar(s,shl(s,x)) lowering as the runtime path (on-chain verified).
	// The earlier conditional-mask form re-read x three times and derived the
	// sign bit from the full x, breaking V4 LiquidityMath.addDelta's
	// `add(and(x,mask), signextend(15, y))` for negative deltas
	// (test: inlineAssembly/signextend_adddelta).
	uint64_t s = 248 - 8 * b;
	auto sStr = std::to_string(s);
	std::vector<std::shared_ptr<awst::Expression>> shlArgs{
		awst::makeIntegerConstant(sStr, _loc, awst::WType::biguintType()), std::move(x)};
	auto shifted = handleShl(shlArgs, _loc);
	std::vector<std::shared_ptr<awst::Expression>> sarArgs{
		awst::makeIntegerConstant(sStr, _loc, awst::WType::biguintType()), std::move(shifted)};
	return handleSar(sarArgs, _loc);
}


} // namespace puyasol::builder
