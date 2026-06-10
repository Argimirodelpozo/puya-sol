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

	// Convert slot arg to uint64 for __storage_read(slot)
	auto slotArg = _args[0];
	if (slotArg->wtype == awst::WType::biguintType())
		slotArg = safeBtoi(std::move(slotArg), _loc);

	// Call __storage_read(slot) → biguint
	auto call = awst::makeSubroutineCall(awst::SubroutineID{"__puyasol___storage_read"}, awst::WType::biguintType(), _loc);

	awst::pushCallArg(call->args, "__slot", std::move(slotArg));

	return call;
}

std::shared_ptr<awst::Expression> AssemblyBuilder::handleGas(
	awst::SourceLocation const& _loc
)
{
	// gas() → global OpcodeBudget, a uint64. Return it as uint64; the consumer
	// coerces via ensureBiguint only when it needs a biguint (match at
	// consumption, not exit — same as number/selfbalance; drops the itob widen).
	Logger::instance().debug(
		"gas() mapped to AVM OpcodeBudget (analogous but not equivalent to EVM gas)", _loc);
	return awst::makeGlobal(std::string("OpcodeBudget"), awst::WType::uint64Type(), _loc);
}

std::shared_ptr<awst::Expression> AssemblyBuilder::handleTimestamp(
	awst::SourceLocation const& _loc
)
{
	// timestamp() → global LatestTimestamp, a uint64. Return it as uint64; the
	// consumer coerces via ensureBiguint only when it needs a biguint (match at
	// consumption, not exit — same as number/selfbalance; drops the itob widen).
	return awst::makeGlobal("LatestTimestamp", awst::WType::uint64Type(), _loc);
}

// ─── New Yul builtins for Uniswap V4 ────────────────────────────────────────

std::shared_ptr<awst::Expression> AssemblyBuilder::buildPowerOf2(
	std::shared_ptr<awst::Expression> _shift,
	awst::SourceLocation const& _loc
)
{
	// Construct 2^shift using setbit(bzero(32), 255-shift, 1)
	// since AVM has no bexp opcode
	auto shiftAmt = _shift;

	// Convert to uint64 if needed (shift amount must be uint64 for subtraction)
	if (shiftAmt->wtype != awst::WType::uint64Type())
	{
		// Cast biguint → bytes first
		auto cast = awst::makeAsBytes(std::move(shiftAmt), _loc);

		// Safe btoi: pad to 8 then extract last 8 (btoi requires ≤8 bytes).
		auto cat = awst::makeLeftPad(std::move(cast), 8, _loc);
		auto extract = awst::makeExtractLastN(std::move(cat), 8, _loc);
		shiftAmt = awst::makeBtoi(std::move(extract), _loc);
	}

	// bzero(32) — 256-bit zero buffer
	auto bzero = awst::makeBzero(32, _loc);

	// Clamp shift amount to 0..255 — EVM shifts mod 256 implicitly,
	// but puya optimizer may strip wrapMod256 from intermediates
	auto twoFiftySix = awst::makeIntegerConstant("256", _loc);

	auto clampedShift = awst::makeUInt64BinOp(std::move(shiftAmt), awst::UInt64BinaryOperator::Mod, std::move(twoFiftySix), _loc);

	// 255 - shift: setbit uses MSB-first ordering, so bit (255-n) = 2^n
	auto twoFiftyFive = awst::makeIntegerConstant("255", _loc);

	auto bitIdx = awst::makeUInt64BinOp(std::move(twoFiftyFive), awst::UInt64BinaryOperator::Sub, std::move(clampedShift), _loc);

	// setbit(bzero(32), 255-shift, 1) → bytes with only bit `shift` set
	auto setbit = awst::makeSetbit(
		std::move(bzero), std::move(bitIdx), awst::makeOne(_loc), _loc);

	// Cast bytes → biguint
	auto castToBigUInt = awst::makeAsBiguint(std::move(setbit), _loc);

	return castToBigUInt;
}

std::shared_ptr<awst::Expression> AssemblyBuilder::handleDiv(
	std::vector<std::shared_ptr<awst::Expression>> const& _args,
	awst::SourceLocation const& _loc
)
{
	if (!checkArity(_args, 2, "div", _loc))
		return nullptr;
	// EVM: div(a, 0) = 0. AVM: b/ by 0 panics.
	// Emit: b != 0 ? a / b : 0
	return safeDivMod(
		_args[0], awst::BigUIntBinaryOperator::FloorDiv, _args[1], _loc
	);
}

std::shared_ptr<awst::Expression> AssemblyBuilder::handleShl(
	std::vector<std::shared_ptr<awst::Expression>> const& _args,
	awst::SourceLocation const& _loc
)
{
	// shl(shift, value) → value * 2^shift  IF shift < 256 ELSE 0
	// NOTE: Yul shl argument order is (shift, value), NOT (value, shift)
	//
	// EVM semantics (EIP-145): shl returns 0 when `shift >= 256` —
	// not `value << (shift mod 256)`. Symmetric to handleShr above.
	if (!checkArity(_args, 2, "shl", _loc))
		return nullptr;
	// Evaluate the shift once — it feeds both buildPowerOf2 and the shift<256
	// guard below (a side-effecting shift would otherwise run twice).
	static int s_shlShiftEvalId = 0;
	auto shift = awst::makeSingleEvaluation(
		ensureBiguint(_args[0], _loc), awst::WType::biguintType(),
		++s_shlShiftEvalId, _loc);
	// Reduce the value to its low 256 bits before shifting. EVM shl operates on
	// a 256-bit word, so (v * 2^s) % 2^256 == ((v % 2^256) * 2^s) % 2^256. Our
	// biguint may carry a wider value — up to 512 bits — e.g. a negative
	// int128 decoded as arc4.uint512 is 2^512 - x; then `v * 2^s` overflows the
	// AVM 64-byte bigint limit *before* the trailing mod can reduce it (the
	// Uniswap V4 Pool.updateTick `shl(128, liquidityNet)` case). Reducing first
	// both bounds the multiply and recovers the correct 256-bit value
	// (2^512 - x ≡ 2^256 - x (mod 2^256)). A no-op for values already <2^256.
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
	// shr(shift, value) → value / 2^shift  IF shift < 256 ELSE 0
	// NOTE: Yul shr argument order is (shift, value), NOT (value, shift)
	//
	// EVM semantics (EIP-145): shr returns 0 when `shift >= 256` —
	// this is NOT `value >> (shift mod 256)`. AVM `b>>` doesn't have
	// the same clamping, so we wrap the result in a conditional.
	// See WIP/examples/aave-v4/contracts/PositionStatusMap.sol:223
	// — `shr(sub(256, ...), MASK)` simplifies to `shr(256, MASK)` at
	// bucket boundaries and must return 0.
	if (!checkArity(_args, 2, "shr", _loc))
		return nullptr;
	// Evaluate the shift once — it feeds both buildPowerOf2 and the shift<256
	// guard below (a side-effecting shift would otherwise run twice).
	static int s_shrShiftEvalId = 0;
	auto shift = awst::makeSingleEvaluation(
		ensureBiguint(_args[0], _loc), awst::WType::biguintType(),
		++s_shrShiftEvalId, _loc);
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
	// byte(n, x) → extract byte n from 32-byte big-endian padded x
	// Implementation: pad x to 32 bytes, then extract3(padded, n, 1)
	if (!checkArity(_args, 2, "byte", _loc))
		return nullptr;

	// Pad x to 32 bytes big-endian
	auto padded = padTo32Bytes(_args[1], _loc);

	// Convert n to uint64 for extract3
	auto nExpr = _args[0];
	if (nExpr->wtype != awst::WType::uint64Type())
	{
		nExpr = safeBtoi(std::move(nExpr), _loc);
	}

	// extract3(padded, n, 1)
	auto one = awst::makeOne(_loc);

	auto extract = awst::makeExtract3(std::move(padded), std::move(nExpr), std::move(one), _loc);
	// Cast bytes → biguint for Yul semantics (all values are uint256)
	auto castResult = awst::makeAsBiguint(std::move(extract), _loc);
	return castResult;
}

std::shared_ptr<awst::Expression> AssemblyBuilder::handleSignextend(
	std::vector<std::shared_ptr<awst::Expression>> const& _args,
	awst::SourceLocation const& _loc
)
{
	// signextend(b, x) — sign-extend x from byte b (0-indexed from low byte).
	// If bit 7 of byte b is set, fill higher bytes with 0xFF, else 0x00.
	//
	// Implementation for two's complement in 256-bit biguint:
	//   bitPos = (b + 1) * 8  (total bits that are significant)
	//   signBit = (x >> (bitPos - 1)) & 1
	//   if signBit:
	//     mask = (2^256 - 1) - (2^bitPos - 1)  // all ones above bitPos
	//     result = x | mask
	//   else:
	//     mask = 2^bitPos - 1   // keep only lower bitPos bits
	//     result = x & mask
	//
	// For simplicity and since V4 uses signextend only in specific patterns,
	// we implement the full logic using conditional expression.
	if (!checkArity(_args, 2, "signextend", _loc))
		return nullptr;

	// Ensure x is biguint
	auto x = ensureBiguint(_args[1], _loc);

	// For constant b values, we can optimize. NOTE: use a STRICT literal check,
	// not resolveConstantOffset — that helper is for memory offsets and recurses
	// into a runtime arg's ABI-decode expression, wrongly folding `b` to the
	// calldata selector offset (4). That made the constant path run for runtime
	// `b`, baking garbage masks. Every real Solidity intN narrowing passes a
	// literal IntegerConstant here, so this still hits the fast path.
	std::optional<uint64_t> bConst;
	if (auto* lit = dynamic_cast<awst::IntegerConstant*>(_args[0].get()))
		bConst = std::stoull(lit->value);
	if (!bConst.has_value())
	{
		// Runtime (non-constant) b. signextend(b, x) == sar(s, shl(s, x)) with
		// s = 248 - 8*min(b, 31). shl moves byte b's sign bit (8*b+7) up to bit
		// 255 (higher bits dropped by shl's mod-2^256 wrap); sar shifts it back,
		// replicating the sign. min(b,31) gives the EVM b>=31 no-op for free
		// (s=0 => sar(0,shl(0,x))=x) and prevents 248-8*b from underflowing.
		// Reuses the on-chain-verified shl/sar lowerings; fresh s per consumer.
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
	{
		// signextend from byte 31 or higher = no-op for 256-bit values
		return x;
	}

	// signextend(b, x) == sar(s, shl(s, x)) with s = 248 - 8*b (a constant here).
	// shl moves byte b's sign bit (8*b+7) up to bit 255 (higher bits dropped by shl's
	// mod-2^256 wrap); sar shifts it back, replicating the sign. This is the SAME
	// on-chain-verified lowering as the runtime-b path, and crucially evaluates x ONCE.
	// The earlier conditional-mask form reused x three times and derived the sign bit
	// from the FULL x; it lowered inconsistently when signextend was composed inside a
	// larger expression — e.g. Uniswap V4 LiquidityMath.addDelta's
	// `add(and(x,mask), signextend(15, y))` for a NEGATIVE delta reverted instead of
	// round-tripping (covered by inlineAssembly/signextend_adddelta).
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
