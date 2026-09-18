/// @file SolIntegerBuilder.cpp
/// Solidity integer type builder — handles all int/uint operations with full
/// Solidity semantics including overflow checking, signed comparison, wrapping.

#include "builder/eb/SolIntegerBuilder.h"
#include "builder/eb/SolBoolBuilder.h"
#include "awst/NameGen.h"
#include "builder/eb/BigUIntMathHelpers.h"
#include "builder/types/TypeCoercion.h"

#include <libsolidity/ast/TypeProvider.h>
#include <libsolutil/Numeric.h>
#include <sstream>

namespace puyasol::builder::eb
{

SolIntegerBuilder::SolIntegerBuilder(
	ContractContext& _ctx,
	solidity::frontend::IntegerType const* _intType,
	std::shared_ptr<awst::Expression> _expr)
	: InstanceBuilder(_ctx, std::move(_expr)),
	  m_intType(_intType),
	  m_int{_intType->numBits(), _intType->isSigned()}
{
}

std::unique_ptr<SolIntegerBuilder> SolIntegerBuilder::wrap(
	std::shared_ptr<awst::Expression> _expr) const
{
	return std::make_unique<SolIntegerBuilder>(m_ctx, m_intType, std::move(_expr));
}

// ─────────────────────────────────────────────────────────────────────
// binary_op
// ─────────────────────────────────────────────────────────────────────

std::unique_ptr<InstanceBuilder> SolIntegerBuilder::binary_op(
	InstanceBuilder& _other, BuilderBinaryOp _op,
	awst::SourceLocation const& _loc)
{
	auto const* otherInt = dynamic_cast<solidity::frontend::IntegerType const*>(_other.solType());
	if (!otherInt)
		return nullptr;

	bool otherIsBigUInt = otherInt->numBits() > 64;
	// Signed sub: always biguint path (uint64 `-` panics on underflow; signed 1-2=-1 is valid).
	// ALL shifts: always biguint path. The raw uint64 `shl`/`shr` opcode FAILS for a shift amount
	// >= 64, but Solidity `x << n` / `x >> n` saturate to 0 (or sign-fill) for n >= the width and
	// never revert — so a sub-word shift by a <=64-bit amount >= 64 (e.g. `uint16 x << 256`, a
	// literal typed <=64-bit so it misses otherIsBigUInt) reverted. buildBigUIntShift /
	// buildBigUIntArithmeticShiftRight saturate correctly; emitOverflowCheck masks to the width
	// (Solidity shifts don't overflow-check). Signed >> additionally needs SAR (sign-fill) here.
	// uint64 UNCHECKED sub: the raw uint64 `-` opcode panics on underflow, but Solidity wraps. The
	// sub-word wrapping (`a + 2^N - b`, below) needs `a + 2^N` to fit uint64 → only m_int.bits<64; uint64
	// (m_int.bits==64) overflows it, so route through the biguint wrapping subtract instead (then narrow).
	bool needsBigUInt = m_int.biguintBacked() || otherIsBigUInt
		|| (m_int.isSigned && _op == BuilderBinaryOp::Sub)
		// Signed div/mod needs the biguint signed path (buildSignedModDiv); otherwise a uint64-backed
		// signed type (int8/16/32/64) falls to the native UNSIGNED uint64 div/mod — wrong for negative
		// operands (e.g. compound int64 -1/int64.min gave 1, not 0). Found by the differential fuzzer.
		|| (m_int.isSigned && (_op == BuilderBinaryOp::FloorDiv || _op == BuilderBinaryOp::Mod))
		|| (m_int.bits == 64 && !m_int.isSigned && m_scope.isUnchecked() && _op == BuilderBinaryOp::Sub)
		|| _op == BuilderBinaryOp::LShift || _op == BuilderBinaryOp::RShift;

	auto lhs = resolve();
	auto rhs = _other.resolve();
	if (_op == BuilderBinaryOp::FloorDiv || _op == BuilderBinaryOp::Mod)
	{
		// Solidity's zero-divisor panic is observable even when the quotient
		// is unused. Keep it explicit: the backend may DCE the arithmetic op.
		auto pin = [&](auto value) { return m_ctx.emitSequencedOperand({}, std::move(value), true, _loc); };
		if (m_ctx.viaIRSequencing) { lhs = pin(std::move(lhs)); rhs = pin(std::move(rhs)); }
		else { rhs = pin(std::move(rhs)); lhs = pin(std::move(lhs)); }
		m_ctx.queuePreExpression(awst::makeAssert(awst::makeNumericCompare(rhs,
			awst::NumericComparison::Ne, awst::makeIntegerConstant("0", _loc, rhs->wtype), _loc),
			_loc, "division by zero (Panic 0x12)"), _loc);
	}

	// These operations reuse operands for sign, range or 0**0 checks.
	if (m_int.isSigned || _op == BuilderBinaryOp::Pow)
	{
		lhs = awst::makeEvalOnce(std::move(lhs), _loc);
		rhs = awst::makeEvalOnce(std::move(rhs), _loc);
	}
	if (m_int.isSigned)
	{
		// Puya's signed-multiply codegen needs a real local for a complex
		// left operand; SingleEvaluation alone can miscount stack slots.
		if (_op == BuilderBinaryOp::Mult
			&& dynamic_cast<awst::SingleEvaluation const*>(lhs.get()))
			lhs = m_ctx.emitSequencedOperand({}, std::move(lhs), true, _loc);
		if (_op == BuilderBinaryOp::Add || _op == BuilderBinaryOp::Sub || _op == BuilderBinaryOp::Mult)
			return wrap(buildSignedArithmetic(m_ctx, m_scope.isUnchecked(), _op,
				std::move(lhs), std::move(rhs), m_int.bits, _loc));
		if (_op == BuilderBinaryOp::Pow)
			return buildSignedPowOp(std::move(lhs), std::move(rhs), _loc);
	}

	// ── BigUInt path: rungs in shape order (shift → sub → pow → signed div/mod → rest) ──
	if (needsBigUInt)
	{
		lhs = promoteToBiguint(std::move(lhs), _loc);

		// Shift amount stays uint64 (don't promote) — the shift rung clamps it.
		if (_op == BuilderBinaryOp::LShift || _op == BuilderBinaryOp::RShift)
			return buildBigUIntShiftOp(_op, std::move(lhs), std::move(rhs), _loc);

		rhs = promoteToBiguint(std::move(rhs), _loc);

		if (_op == BuilderBinaryOp::Sub)
			return buildBigUIntSubOp(std::move(lhs), std::move(rhs), _loc);
		if (_op == BuilderBinaryOp::Pow)
			return buildBigUIntPowOp(std::move(lhs), std::move(rhs), _loc);
		if (m_int.isSigned && (_op == BuilderBinaryOp::Mod || _op == BuilderBinaryOp::FloorDiv))
			return buildSignedModDivOp(_op, std::move(lhs), std::move(rhs),
				m_int.bits,
				otherInt->numBits(), _loc);
		return buildBigUIntArithBitwiseOp(_op, std::move(lhs), std::move(rhs), _loc);
	}

	// Widen whenever the intermediate can overflow AVM uint64, then wrap to
	// solc's result width. Products of uint40/48/56 need this too.
	if (m_scope.isUnchecked() && !m_int.isSigned
		&& ((_op == BuilderBinaryOp::Add && m_int.bits == 64)
			|| (_op == BuilderBinaryOp::Mult && m_int.bits + otherInt->numBits() > 64)))
		return buildUInt64WrappingAddMult(_op, std::move(lhs), std::move(rhs), _loc);
	return buildUInt64ArithBitwiseOp(_op, std::move(lhs), std::move(rhs), _loc);
}

// ── binary_op rungs ──────────────────────────────────────────────────

std::unique_ptr<InstanceBuilder> SolIntegerBuilder::buildBigUIntShiftOp(
	BuilderBinaryOp _op, std::shared_ptr<awst::Expression> _lhs,
	std::shared_ptr<awst::Expression> _rhs, awst::SourceLocation const& _loc)
{
	// Shift amount stays uint64 (don't promote) — but a biguint-typed amount
	// must be CLAMPED, not low-64-truncated: `x >> 2^128` shifted by 0 where
	// EVM saturates for any amount >= 256. shiftAmountToUint64 selects 256
	// for huge amounts, which the shift builders below saturate on.
	auto shiftAmt = shiftAmountToUint64(std::move(_rhs), _loc);
	std::shared_ptr<awst::Expression> result;
	// Signed >> = SAR (sign-filling); logical FloorDiv would zero-fill negatives.
	if (m_int.isSigned && _op == BuilderBinaryOp::RShift)
	{
		// Sub-word signed: canonicalize to 256-bit two's complement FIRST, so a shift by
		// >= the value's own width still sign-fills (int8(-1) >> 256 == -1, not 0). The
		// value is only 8/64-bit-wide as a local/param, so without this the SAR's
		// negativity test (v >= 2^255) is false and it zero-fills.
		if (m_int.bits < 256)
			_lhs = TypeCoercion::signExtendToUint256(std::move(_lhs), m_int.bits, _loc);
		result = buildBigUIntArithmeticShiftRight(std::move(_lhs), std::move(shiftAmt), _loc);
	}
	else
		result = buildBigUIntShift(std::move(_lhs), std::move(shiftAmt),
			_op == BuilderBinaryOp::LShift, _loc);
	// Solidity truncates `x << n` to the type width — shifts never overflow-check, even when
	// checked — but buildBigUIntShift only wraps to 2^256. Mask unsigned sub-word/uint64
	// LShift back to 2^bits (`uint8(254) << 1` is 252, not 508). RShift only shrinks the
	// value so it always already fits. Found by the differential fuzzer.
	if (_op == BuilderBinaryOp::LShift && !m_int.isSigned && m_int.bits < 256)
		result = TypeCoercion::maskUnsignedToWidth(std::move(result), m_int.bits, _loc);
	if (m_int.isSigned)
		result = TypeCoercion::coerceToCommonInt(
			std::move(result), m_intType, awst::WType::biguintType(), _loc);
	result = emitOverflowCheck(std::move(result), _op, _loc);
	// A sub-word value's native WType is uint64; narrow the biguint shift result back so
	// it composes as a SUB-expression with surrounding uint64 ops — `(a << 7) & b` else
	// hands a biguint to a UInt64BinaryOperation (puya: "expected uint64"). The value is
	// masked/sign-extended to <=64 bits, so the cast is lossless. >64-bit stays biguint.
	if (!m_int.biguintBacked() && result->wtype == awst::WType::biguintType())
		result = TypeCoercion::coerceScalar(
			std::move(result), awst::WType::uint64Type(), _loc);
	return wrap(std::move(result));
}

std::unique_ptr<InstanceBuilder> SolIntegerBuilder::buildBigUIntSubOp(
	std::shared_ptr<awst::Expression> _lhs, std::shared_ptr<awst::Expression> _rhs,
	awst::SourceLocation const& _loc)
{
	// Signed: skip `a>=b` assert — `1-2=-1` is valid two's complement, not underflow.
	bool skipUnsignedAssert = m_int.isSigned || m_scope.isUnchecked();
	auto result = buildWrappingSubtract(m_ctx, skipUnsignedAssert, std::move(_lhs), std::move(_rhs), _loc);
	result = emitOverflowCheck(std::move(result), BuilderBinaryOp::Sub, _loc);
	// Unchecked unsigned sub-256 biguint underflow wraps to 2^256 (buildWrappingSubtract),
	// but Solidity wraps to 2^N: `uint128(0) - 1` is 2^128-1, not 2^256-1. Mask to the type
	// width so checked consumers and `<= uintN.max` don't see a non-canonical value. (uint64
	// narrows below; checked sub asserted a>=b so its result is in range; signed keeps 256-bit
	// two's complement.) Found by the differential fuzzer.
	if (m_scope.isUnchecked() && !m_int.isSigned && m_int.biguintBacked() && m_int.bits < 256)
		result = TypeCoercion::maskUnsignedToWidth(std::move(result), m_int.bits, _loc);
	// uint64 routed here for unchecked-underflow wrapping (above): the 256-bit wrap narrows
	// to uint64 = the correct mod-2^64 value, and composes with surrounding uint64 ops.
	if (!m_int.biguintBacked() && result->wtype == awst::WType::biguintType())
		result = TypeCoercion::coerceScalar(
			std::move(result), awst::WType::uint64Type(), _loc);
	return wrap(std::move(result));
}

std::unique_ptr<InstanceBuilder> SolIntegerBuilder::buildBigUIntPowOp(
	std::shared_ptr<awst::Expression> _lhs, std::shared_ptr<awst::Expression> _rhs,
	awst::SourceLocation const& _loc)
{
	auto result = buildBigUIntExp(m_ctx, m_scope.isUnchecked(), std::move(_lhs), std::move(_rhs), _loc);
	result = emitOverflowCheck(std::move(result), BuilderBinaryOp::Pow, _loc);
	// Unchecked sub-256 biguint exp wraps products mod 2^256 (buildBigUIntExp), but Solidity
	// wraps to 2^N: e.g. `uint128 a ** 2` must be mod 2^128. Mask to the type width (same as the
	// unchecked sub fix above). Found by the differential fuzzer.
	if (m_scope.isUnchecked() && !m_int.isSigned && m_int.bits < 256)
		result = TypeCoercion::maskUnsignedToWidth(std::move(result), m_int.bits, _loc);
	// A wide exponent does not widen the result: solc keeps the base's type.
	if (!m_int.biguintBacked())
		result = TypeCoercion::coerceScalar(std::move(result), awst::WType::uint64Type(), _loc);
	return wrap(std::move(result));
}

std::unique_ptr<InstanceBuilder> SolIntegerBuilder::buildSignedPowOp(
	std::shared_ptr<awst::Expression> base, std::shared_ptr<awst::Expression> exponent,
	awst::SourceLocation const& loc)
{
	auto const [modulus, half] = TypeCoercion::pow2NAndHalf(m_int.bits);
	auto constant = [&](std::string const& value) {
		return awst::makeIntegerConstant(value, loc, awst::WType::biguintType());
	};
	base = promoteToBiguint(std::move(base), loc);
	exponent = promoteToBiguint(std::move(exponent), loc);
	if (m_int.bits < 256)
		base = awst::makeBigUIntBinOp(std::move(base), awst::BigUIntBinaryOperator::Mod, constant(modulus), loc);
	auto negative = awst::makeNumericCompare(base, awst::NumericComparison::Gte, constant(half), loc);
	auto magnitude = awst::makeConditional(negative,
		awst::makeBigUIntBinOp(constant(modulus), awst::BigUIntBinaryOperator::Sub, base, loc),
		base, awst::WType::biguintType(), loc);
	auto odd = awst::makeNumericCompare(awst::makeBigUIntBinOp(exponent,
		awst::BigUIntBinaryOperator::Mod, constant("2"), loc),
		awst::NumericComparison::Ne, constant("0"), loc);
	auto resultNegative = awst::makeBoolBinOp(negative, awst::BinaryBooleanOperator::And, odd, loc);
	auto result = buildBigUIntExp(m_ctx, m_scope.isUnchecked(), std::move(magnitude), exponent, loc);
	if (m_scope.isUnchecked())
	{
		// Wrap before negation, which otherwise underflows for an overflowing magnitude.
		if (m_int.bits < 256)
			result = awst::makeBigUIntBinOp(std::move(result),
				awst::BigUIntBinaryOperator::Mod, constant(modulus), loc);
	}
	else
	{
		auto inRange = awst::makeConditional(resultNegative,
			awst::makeNumericCompare(result, awst::NumericComparison::Lte, constant(half), loc),
			awst::makeNumericCompare(result, awst::NumericComparison::Lt, constant(half), loc),
			awst::WType::boolType(), loc);
		m_ctx.queuePreExpression(awst::makeAssert(inRange, loc, "signed exp overflow"), loc);
	}
	auto negate = awst::makeBoolBinOp(resultNegative, awst::BinaryBooleanOperator::And,
		awst::makeNumericCompare(result, awst::NumericComparison::Ne, constant("0"), loc), loc);
	result = awst::makeConditional(negate,
		awst::makeBigUIntBinOp(constant(modulus), awst::BigUIntBinaryOperator::Sub, result, loc),
		result, awst::WType::biguintType(), loc);
	// Every consumer receives the native carrier's canonical two's complement,
	// including when the power is a subexpression rather than a whole return.
	if (m_int.bits < 256)
		result = TypeCoercion::signExtendToUint256(std::move(result), m_int.bits, loc);
	if (!m_int.biguintBacked())
		result = TypeCoercion::coerceScalar(std::move(result), awst::WType::uint64Type(), loc);
	return wrap(std::move(result));
}

std::unique_ptr<InstanceBuilder> SolIntegerBuilder::buildSignedModDivOp(
	BuilderBinaryOp _op, std::shared_ptr<awst::Expression> _lhs,
	std::shared_ptr<awst::Expression> _rhs, unsigned _lhsBits, unsigned _rhsBits,
	awst::SourceLocation const& _loc)
{
	// buildSignedModDiv needs canonical 256-bit two's complement (it reads sign from
	// `value >= 2^255`). promoteToBiguint above ZERO-extends, so a narrower signed
	// operand (e.g. int16 -32768 -> 2^64-32768) would read as a huge POSITIVE number
	// -> wrong abs/sign. Sign-extend each from its own width (idempotent for canonical
	// int128/int256). The div-overflow guard + result narrowing now live INSIDE the
	// shared helper (same path as direct `a/b` — see BigUIntMathHelpers).
	if (_lhsBits < 256)
		_lhs = TypeCoercion::signExtendToUint256(std::move(_lhs), _lhsBits, _loc);
	if (_rhsBits < 256)
		_rhs = TypeCoercion::signExtendToUint256(std::move(_rhs), _rhsBits, _loc);
	return wrap(buildSignedModDiv(
		std::move(_lhs), std::move(_rhs), _op, m_int.bits, !m_scope.isUnchecked(), _loc));
}

std::unique_ptr<InstanceBuilder> SolIntegerBuilder::buildBigUIntArithBitwiseOp(
	BuilderBinaryOp _op, std::shared_ptr<awst::Expression> _lhs,
	std::shared_ptr<awst::Expression> _rhs, awst::SourceLocation const& _loc)
{
	awst::BigUIntBinaryOperator bigOp = awst::BigUIntBinaryOperator::Add;
	switch (_op)
	{
	case BuilderBinaryOp::Add: bigOp = awst::BigUIntBinaryOperator::Add; break;
	case BuilderBinaryOp::Mult: bigOp = awst::BigUIntBinaryOperator::Mult; break;
	case BuilderBinaryOp::FloorDiv: bigOp = awst::BigUIntBinaryOperator::FloorDiv; break;
	case BuilderBinaryOp::Mod: bigOp = awst::BigUIntBinaryOperator::Mod; break;
	case BuilderBinaryOp::BitOr: bigOp = awst::BigUIntBinaryOperator::BitOr; break;
	case BuilderBinaryOp::BitXor: bigOp = awst::BigUIntBinaryOperator::BitXor; break;
	case BuilderBinaryOp::BitAnd: bigOp = awst::BigUIntBinaryOperator::BitAnd; break;
	default: break;
	}
	auto e = awst::makeBigUIntBinOp(std::move(_lhs), bigOp, std::move(_rhs), _loc);

	std::shared_ptr<awst::Expression> result = e;

	if (m_scope.isUnchecked()
		&& (_op == BuilderBinaryOp::Add || _op == BuilderBinaryOp::Mult))
	{
		// Wrap to the TYPE width (mod 2^m_int.bits), not mod 2^256. A sub-256 unchecked
		// Add/Mult can exceed 2^m_int.bits (e.g. uint128 2*(2^128-1)) yet stay < 2^256, so
		// wrapMod256 left it non-canonical — correct when the value is masked again at
		// the ARC4 encode, but wrong when consumed first (e.g. `(a*~c)/x` divides a
		// too-wide dividend). Mirrors the unchecked sub/exp masking above.
		result = (m_int.bits < 256)
			? TypeCoercion::maskUnsignedToWidth(std::move(result), m_int.bits, _loc)
			: wrapMod256(std::move(result), _loc);
	}

	return wrap(emitOverflowCheck(std::move(result), _op, _loc));
}

std::unique_ptr<InstanceBuilder> SolIntegerBuilder::buildUInt64WrappingAddMult(
	BuilderBinaryOp _op, std::shared_ptr<awst::Expression> _lhs,
	std::shared_ptr<awst::Expression> _rhs, awst::SourceLocation const& _loc)
{
	// Compute without uint64 overflow, then wrap to the declared width.
	auto lb = promoteToBiguint(std::move(_lhs), _loc);
	auto rb = promoteToBiguint(std::move(_rhs), _loc);
	auto big = awst::makeBigUIntBinOp(std::move(lb),
		_op == BuilderBinaryOp::Add ? awst::BigUIntBinaryOperator::Add
			: awst::BigUIntBinaryOperator::Mult,
		std::move(rb), _loc);
	auto mod = TypeCoercion::maskUnsignedToWidth(std::move(big), m_int.bits, _loc);
	return wrap(TypeCoercion::coerceScalar(std::move(mod), awst::WType::uint64Type(), _loc));
}

std::unique_ptr<InstanceBuilder> SolIntegerBuilder::buildUInt64PowOp(
	std::shared_ptr<awst::UInt64BinaryOperation> _e, awst::SourceLocation const& _loc)
{
	// Unchecked uint exp: AVM `exp` is uint64-only and asserts on overflow; both a sub-uint64
	// intermediate (uint8 2**256) AND a full uint64 base whose power overflows 2^64 (uint64
	// MAX**2, found by the generative cast fuzzer) would revert where Solidity wraps. Route
	// through biguint square-and-multiply then mod 2**m_int.bits. Add/Mult/Sub at uint64 already wrap
	// (needsBigUInt / backend); exp is the one that fell in the m_int.bits<64 gap (== the uint64-sub gap).
	if (m_scope.isUnchecked() && !m_int.isSigned && m_int.bits <= 64)
		return buildBigUIntPowOp(std::move(_e->left), std::move(_e->right), _loc);

	// AVM `exp` asserts on 0^0; Solidity defines 0**0=1.
	_e->op = awst::UInt64BinaryOperator::Pow;

	auto zero = awst::makeZero(_loc);

	auto cond = awst::makeNumericCompare(_e->right, awst::NumericComparison::Eq, std::move(zero), _loc);

	auto one = awst::makeOne(_loc);

	std::shared_ptr<awst::Expression> powResult = awst::makeConditional(
		std::move(cond), std::move(one), _e, awst::WType::uint64Type(), _loc);

	return wrap(emitOverflowCheck(std::move(powResult), BuilderBinaryOp::Pow, _loc));
}

std::unique_ptr<InstanceBuilder> SolIntegerBuilder::buildUInt64ArithBitwiseOp(
	BuilderBinaryOp _op, std::shared_ptr<awst::Expression> _lhs,
	std::shared_ptr<awst::Expression> _rhs, awst::SourceLocation const& _loc)
{
	auto e = std::make_shared<awst::UInt64BinaryOperation>();
	e->sourceLocation = _loc;
	e->wtype = awst::WType::uint64Type();
	e->left = std::move(_lhs);
	e->right = std::move(_rhs);

	switch (_op)
	{
	case BuilderBinaryOp::Add: e->op = awst::UInt64BinaryOperator::Add; break;
	case BuilderBinaryOp::Sub:
	{
		// Unchecked narrow uint sub: AVM `-` panics on underflow; use (a+2^N-b)%2^N.
		if (m_scope.isUnchecked() && !m_int.isSigned && m_int.bits < 64)
		{
			uint64_t pow2N = uint64_t(1) << m_int.bits;
			auto powConst = awst::makeIntegerConstant(pow2N, _loc);

			auto aPlusPow = awst::makeUInt64BinOp(std::move(e->left), awst::UInt64BinaryOperator::Add, std::move(powConst), _loc);

			e->left = std::move(aPlusPow);
			}
		e->op = awst::UInt64BinaryOperator::Sub;
		break;
	}
	case BuilderBinaryOp::Mult: e->op = awst::UInt64BinaryOperator::Mult; break;
	case BuilderBinaryOp::FloorDiv: e->op = awst::UInt64BinaryOperator::FloorDiv; break;
	case BuilderBinaryOp::Mod: e->op = awst::UInt64BinaryOperator::Mod; break;
	case BuilderBinaryOp::Pow:
		// Exp is its own rung: both of its shapes return from inside it.
		return buildUInt64PowOp(std::move(e), _loc);
	case BuilderBinaryOp::LShift: e->op = awst::UInt64BinaryOperator::LShift; break;
	case BuilderBinaryOp::RShift: e->op = awst::UInt64BinaryOperator::RShift; break;
	case BuilderBinaryOp::BitOr: e->op = awst::UInt64BinaryOperator::BitOr; break;
	case BuilderBinaryOp::BitXor: e->op = awst::UInt64BinaryOperator::BitXor; break;
	case BuilderBinaryOp::BitAnd: e->op = awst::UInt64BinaryOperator::BitAnd; break;
	}

	std::shared_ptr<awst::Expression> result = e;

	if (m_scope.isUnchecked() && !m_int.isSigned && m_int.bits < 64)
	{
		bool needsWrap = (_op == BuilderBinaryOp::Add || _op == BuilderBinaryOp::Sub
			|| _op == BuilderBinaryOp::Mult || _op == BuilderBinaryOp::Pow);
		if (needsWrap)
		{
			uint64_t modVal = uint64_t(1) << m_int.bits;
			auto modConst = awst::makeIntegerConstant(modVal, _loc);

			auto masked = awst::makeUInt64BinOp(std::move(result), awst::UInt64BinaryOperator::Mod, std::move(modConst), _loc);
			result = std::move(masked);
		}
	}

	return wrap(emitOverflowCheck(std::move(result), _op, _loc));
}

// ─────────────────────────────────────────────────────────────────────
// compare — includes signed comparison via XOR with sign bit
// ─────────────────────────────────────────────────────────────────────

std::unique_ptr<InstanceBuilder> SolIntegerBuilder::compare(
	InstanceBuilder& _other, BuilderComparisonOp _op,
	awst::SourceLocation const& _loc)
{
	auto const* otherInt = dynamic_cast<solidity::frontend::IntegerType const*>(_other.solType());
	if (!otherInt)
		return nullptr;

	bool otherIsBigUInt = otherInt->numBits() > 64;
	bool needsBigUInt = m_int.biguintBacked() || otherIsBigUInt;
	bool isSigned = m_int.isSigned || otherInt->isSigned();

	auto lhs = resolve();
	auto rhs = _other.resolve();

	// Operands arrive coerced to the op's commonType (same width + wtype, already
	// canonical) from SolBinaryOperation's coerceToCommonInt, so the old
	// narrowConstIfNegative const-narrowing and per-operand sign-extension here are
	// unnecessary — only the biguint promotion (cheap no-op when already biguint) and
	// the signed-ordering sign-bit XOR remain.

	if (needsBigUInt)
	{
		lhs = promoteToBiguint(std::move(lhs), _loc);
		rhs = promoteToBiguint(std::move(rhs), _loc);
	}

	bool isOrderingOp = (_op == BuilderComparisonOp::Lt || _op == BuilderComparisonOp::Lte
		|| _op == BuilderComparisonOp::Gt || _op == BuilderComparisonOp::Gte);

	// Ordering additionally XORs the sign bit to convert signed → unsigned ordering
	// (operands are already canonical above; equality compares them directly).
	if (isSigned && isOrderingOp)
	{
		if (needsBigUInt)
		{
			solidity::u256 signBitVal = solidity::u256(1) << 255;
			auto signBit = awst::makeIntegerConstant(signBitVal.str(), _loc, awst::WType::biguintType());

			auto xorL = awst::makeBigUIntBinOp(std::move(lhs), awst::BigUIntBinaryOperator::BitXor, signBit, _loc);
			lhs = std::move(xorL);

			auto signBit2 = awst::makeIntegerConstant(signBitVal.str(), _loc, awst::WType::biguintType());

			auto xorR = awst::makeBigUIntBinOp(std::move(rhs), awst::BigUIntBinaryOperator::BitXor, std::move(signBit2), _loc);
			rhs = std::move(xorR);
		}
		else
		{
			auto signBit = awst::makeIntegerConstant("9223372036854775808", _loc); // 2^63

			auto xorL = awst::makeUInt64BinOp(std::move(lhs), awst::UInt64BinaryOperator::BitXor, signBit, _loc);
			lhs = std::move(xorL);

			auto signBit2 = awst::makeIntegerConstant("9223372036854775808", _loc);

			auto xorR = awst::makeUInt64BinOp(std::move(rhs), awst::UInt64BinaryOperator::BitXor, std::move(signBit2), _loc);
			rhs = std::move(xorR);
		}
	}

	if (lhs->wtype != rhs->wtype)
	{
		if (lhs->wtype == awst::WType::uint64Type() && rhs->wtype == awst::WType::biguintType())
			lhs = promoteToBiguint(std::move(lhs), _loc);
		else if (rhs->wtype == awst::WType::uint64Type() && lhs->wtype == awst::WType::biguintType())
			rhs = promoteToBiguint(std::move(rhs), _loc);
	}


	auto cmp = awst::makeNumericCompare(std::move(lhs), _op, std::move(rhs), _loc);

	return std::make_unique<SolBoolBuilder>(m_ctx, std::move(cmp));
}

// ─────────────────────────────────────────────────────────────────────
// unary_op
// ─────────────────────────────────────────────────────────────────────

std::unique_ptr<InstanceBuilder> SolIntegerBuilder::unary_op(
	BuilderUnaryOp _op, awst::SourceLocation const& _loc)
{
	if (_op != BuilderUnaryOp::Negative && _op != BuilderUnaryOp::BitInvert)
		return nullptr;

	auto* carrier = m_int.biguintBacked()
		? awst::WType::biguintType() : awst::WType::uint64Type();
	auto operand = awst::makeEvalOnce(
		TypeCoercion::coerceToCommonInt(resolve(), m_intType, carrier, _loc), _loc);
	std::shared_ptr<awst::Expression> result;
	if (_op == BuilderUnaryOp::Negative)
	{
		// solc checks the declared signed width, not just the AVM carrier.
		// Normalize first: a narrow shift/complement can have dirty upper bits.
		if (m_int.isSigned && !m_scope.isUnchecked())
		{
			solidity::u256 min = solidity::u256(0) - (solidity::u256(1) << (m_int.bits - 1));
			auto check = awst::makeNumericCompare(operand, awst::NumericComparison::Ne,
				TypeCoercion::canonicalIntConstant(min, m_int.biguintBacked() ? 256 : 64, _loc), _loc);
			m_ctx.queuePreExpression(awst::makeAssert(check, _loc, "signed negation overflow"), _loc);
		}
		auto modulus = m_int.biguintBacked() ? makePow256(_loc)
			: awst::makeBiguintConstant("18446744073709551616", _loc);
		result = awst::makeBigUIntBinOp(
			awst::makeBigUIntBinOp(modulus, awst::BigUIntBinaryOperator::Sub,
				promoteToBiguint(std::move(operand), _loc), _loc),
			awst::BigUIntBinaryOperator::Mod, modulus, _loc);
	}
	else if (m_int.biguintBacked())
	{
		// AVM b~ only inverts actual bytes; first materialize the full word.
		result = awst::makeAsBiguint(awst::makeBitInvert(
			awst::makeLeftPadToN(awst::makeAsBytes(std::move(operand), _loc), 32, _loc),
			awst::WType::bytesType(), _loc), _loc);
		if (!m_int.isSigned && m_int.bits < 256)
			result = TypeCoercion::maskUnsignedToWidth(std::move(result), m_int.bits, _loc);
	}
	else
	{
		auto mask = m_int.bits == 64 ? UINT64_MAX : (uint64_t(1) << m_int.bits) - 1;
		result = awst::makeUInt64BinOp(std::move(operand), awst::UInt64BinaryOperator::BitXor,
			awst::makeIntegerConstant(mask, _loc), _loc);
	}
	return wrap(TypeCoercion::coerceToCommonInt(std::move(result), m_intType, carrier, _loc));
}

// ─────────────────────────────────────────────────────────────────────
// Overflow checking
// ─────────────────────────────────────────────────────────────────────

std::shared_ptr<awst::Expression> SolIntegerBuilder::emitOverflowCheck(
	std::shared_ptr<awst::Expression> _result,
	BuilderBinaryOp _op,
	awst::SourceLocation const& _loc)
{
	if (m_scope.isUnchecked())
		return _result;

	bool needsCheck = (_op == BuilderBinaryOp::Add || _op == BuilderBinaryOp::Sub
		|| _op == BuilderBinaryOp::Mult || _op == BuilderBinaryOp::Pow);
	if (!needsCheck || m_int.isSigned)
		return _result;

	unsigned maxBits = m_int.biguintBacked() ? 256 : 64;
	// uint64 (native): the AVM +/*/exp opcodes revert on overflow themselves, so no explicit check
	// at the max width. BigUInt (uint65..uint256): does NOT auto-revert at 2^bits — biguint is
	// arbitrary precision (up to the AVM 512-bit cap), so the result of `s+1` at uint256 is the
	// exact 2^256, not a wrapped 0. It MUST be checked at every width INCLUDING 256; otherwise
	// `uint64(s + 1)` truncates that 2^256 to 0 before any downstream (return/store) check sees it,
	// silently wrapping instead of reverting. Found by the differential fuzzer.
	if (m_int.bits >= maxBits && !m_int.biguintBacked())
		return _result;

	std::string tmpName = "__checked_" + std::to_string(awst::NameGen::next("SolIntegerBuilder.checkedCounter"));
	auto* resType = _result->wtype;

	std::string maxValStr;
	if (m_int.biguintBacked())
		maxValStr = ((solidity::u256(1) << m_int.bits) - 1).str();
	else
		maxValStr = std::to_string((uint64_t(1) << m_int.bits) - 1);

	auto mkCmp = [&]() {
		return awst::makeNumericCompare(
			awst::makeVarExpression(tmpName, resType, _loc), awst::NumericComparison::Lte,
			awst::makeIntegerConstant(std::string(maxValStr), _loc, resType), _loc);
	};

	// Biguint-backed (65..256): emit the check INLINE as a comma expression,
	// not as pre-statements. These ops reach emitOverflowCheck in modifier-arg
	// / constructor / return-expression contexts that don't flush
	// the current pre-effect frame at the right point, so a pre-statement check is
	// mis-placed there (regressed g()'s `r+r` modifier args etc.). A comma
	// `(t=res, assert, t)` is a pure value expression and composes anywhere.
	// Was gated on bits==256 only; uint65..255 had the same mis-placement.
	if (m_int.biguintBacked())
	{
		auto bind = awst::makeAssignmentExpression(
			awst::makeVarExpression(tmpName, resType, _loc), std::move(_result), _loc, resType);
		auto assertExpr = awst::makeAssert(mkCmp(), _loc, "overflow");
		auto comma = awst::makeCommaExpression(resType, _loc);
		comma->expressions.push_back(std::move(bind));
		comma->expressions.push_back(std::move(assertExpr));
		comma->expressions.push_back(awst::makeVarExpression(tmpName, resType, _loc));
		return comma;
	}

	auto tmpVar = awst::makeVarExpression(tmpName, resType, _loc);
	auto assign = awst::makeAssignmentStatement(tmpVar, std::move(_result), _loc);
	m_ctx.preEffects().push_back(std::move(assign));
	auto assertStmt = awst::makeExpressionStatement(awst::makeAssert(mkCmp(), _loc, "overflow"), _loc);
	m_ctx.preEffects().push_back(std::move(assertStmt));
	return tmpVar;
}

} // namespace puyasol::builder::eb
