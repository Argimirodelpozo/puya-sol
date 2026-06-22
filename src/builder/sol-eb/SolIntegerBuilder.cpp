/// @file SolIntegerBuilder.cpp
/// Solidity integer type builder — handles all int/uint operations with full
/// Solidity semantics including overflow checking, signed comparison, wrapping.

#include "builder/sol-eb/SolIntegerBuilder.h"
#include "builder/sol-eb/BigUIntMathHelpers.h"
#include "builder/sol-types/TypeCoercion.h"

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
	  m_bits(_intType->numBits()),
	  m_signed(_intType->isSigned()),
	  m_isBigUInt(_intType->numBits() > 64)
{
}

std::unique_ptr<SolIntegerBuilder> SolIntegerBuilder::wrap(
	std::shared_ptr<awst::Expression> _expr) const
{
	return std::make_unique<SolIntegerBuilder>(m_ctx, m_intType, std::move(_expr));
}

// ─────────────────────────────────────────────────────────────────────
// Promotion helpers
// ─────────────────────────────────────────────────────────────────────

std::shared_ptr<awst::Expression> SolIntegerBuilder::promoteToBigUInt(
	std::shared_ptr<awst::Expression> _expr,
	awst::SourceLocation const& _loc)
{
	if (_expr->wtype == awst::WType::biguintType())
		return _expr;

	// Integer constants: biguint constant directly (avoids itob(0)=8 zero bytes).
	if (auto const* intConst = dynamic_cast<awst::IntegerConstant const*>(_expr.get()))
	{
		auto bigConst = awst::makeIntegerConstant(intConst->value, _loc, awst::WType::biguintType());
		return bigConst;
	}

	// account/bytes: reinterpret directly; itob is uint64-only and truncates >8 bytes.
	// Mirrors AssemblyBuilder::ensureBiguint (`01332f363`).
	if (_expr->wtype == awst::WType::accountType()
		|| (_expr->wtype && _expr->wtype->kind() == awst::WTypeKind::Bytes))
	{
		auto bytesExpr = _expr->wtype == awst::WType::accountType()
			? awst::makeAsBytes(std::move(_expr), _loc)
			: std::move(_expr);
		return awst::makeAsBiguint(std::move(bytesExpr), _loc);
	}

	auto itob = awst::makeItob(std::move(_expr), _loc);
	return awst::makeAsBiguint(std::move(itob), _loc);
}

// ─────────────────────────────────────────────────────────────────────
// binary_op
// ─────────────────────────────────────────────────────────────────────

std::unique_ptr<InstanceBuilder> SolIntegerBuilder::binary_op(
	InstanceBuilder& _other, BuilderBinaryOp _op,
	awst::SourceLocation const& _loc, bool _reverse)
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
	// sub-word wrapping (`a + 2^N - b`, below) needs `a + 2^N` to fit uint64 → only m_bits<64; uint64
	// (m_bits==64) overflows it, so route through the biguint wrapping subtract instead (then narrow).
	bool needsBigUInt = m_isBigUInt || otherIsBigUInt
		|| (m_signed && _op == BuilderBinaryOp::Sub)
		|| (m_bits == 64 && !m_signed && m_scope.isUnchecked() && _op == BuilderBinaryOp::Sub)
		|| _op == BuilderBinaryOp::LShift || _op == BuilderBinaryOp::RShift;

	auto lhs = resolve();
	auto rhs = _other.resolve();
	if (_reverse)
		std::swap(lhs, rhs);

	// Signed add/sub/mul: route through the shared signed-arithmetic helper (mod 2^N two's
	// complement + signed-overflow check + sub-256 canonicalisation). The biguint/uint64 paths
	// below are UNSIGNED — fine for `a+b` (SolBinaryOperation uses the same helper directly) but the
	// COMPOUND path (`x+=d`) reaches binary_op here and otherwise mis-lowered signed assignment
	// (int128 `x+=1` false-reverted, real overflow wrapped to untruncated garbage).
	if (m_signed && (_op == BuilderBinaryOp::Add || _op == BuilderBinaryOp::Sub
			|| _op == BuilderBinaryOp::Mult))
		return wrap(buildSignedArithmetic(m_ctx, m_scope.isUnchecked(), _op,
			std::move(lhs), std::move(rhs), m_bits, _loc));

	// ── BigUInt path ──
	if (needsBigUInt)
	{
		lhs = promoteToBigUInt(std::move(lhs), _loc);

		// Shift amount stays uint64 (don't promote).
		if (_op == BuilderBinaryOp::LShift || _op == BuilderBinaryOp::RShift)
		{
			auto shiftAmt = TypeCoercion::implicitNumericCast(
				std::move(rhs), awst::WType::uint64Type(), _loc);
			std::shared_ptr<awst::Expression> result;
			// Signed >> = SAR (sign-filling); logical FloorDiv would zero-fill negatives.
			if (m_signed && _op == BuilderBinaryOp::RShift)
			{
				// Sub-word signed: canonicalize to 256-bit two's complement FIRST, so a shift by
				// >= the value's own width still sign-fills (int8(-1) >> 256 == -1, not 0). The
				// value is only 8/64-bit-wide as a local/param, so without this the SAR's
				// negativity test (v >= 2^255) is false and it zero-fills.
				if (m_bits < 256)
					lhs = TypeCoercion::signExtendToUint256(std::move(lhs), m_bits, _loc);
				result = buildBigUIntArithmeticShiftRight(std::move(lhs), std::move(shiftAmt), _loc);
			}
			else
				result = buildBigUIntShift(std::move(lhs), std::move(shiftAmt),
					_op == BuilderBinaryOp::LShift, _loc);
			result = emitOverflowCheck(std::move(result), _op, _loc);
			// A sub-word value's native WType is uint64; narrow the biguint shift result back so
			// it composes as a SUB-expression with surrounding uint64 ops — `(a << 7) & b` else
			// hands a biguint to a UInt64BinaryOperation (puya: "expected uint64"). The value is
			// masked/sign-extended to <=64 bits, so the cast is lossless. >64-bit stays biguint.
			if (!m_isBigUInt && result->wtype == awst::WType::biguintType())
				result = TypeCoercion::implicitNumericCast(
					std::move(result), awst::WType::uint64Type(), _loc);
			return wrap(std::move(result));
		}

		rhs = promoteToBigUInt(std::move(rhs), _loc);

		if (_op == BuilderBinaryOp::Sub)
		{
			// Signed: skip `a>=b` assert — `1-2=-1` is valid two's complement, not underflow.
			bool skipUnsignedAssert = m_signed || m_scope.isUnchecked();
			auto result = buildWrappingSubtract(m_ctx, skipUnsignedAssert, std::move(lhs), std::move(rhs), _loc);
			result = emitOverflowCheck(std::move(result), _op, _loc);
			// uint64 routed here for unchecked-underflow wrapping (above): the 256-bit wrap narrows
			// to uint64 = the correct mod-2^64 value, and composes with surrounding uint64 ops.
			if (!m_isBigUInt && result->wtype == awst::WType::biguintType())
				result = TypeCoercion::implicitNumericCast(
					std::move(result), awst::WType::uint64Type(), _loc);
			return wrap(std::move(result));
		}

		if (_op == BuilderBinaryOp::Pow)
		{
			auto result = buildBigUIntExp(m_ctx, m_scope.isUnchecked(), std::move(lhs), std::move(rhs), _loc);
			return wrap(emitOverflowCheck(std::move(result), _op, _loc));
		}

		if (m_signed && (_op == BuilderBinaryOp::Mod || _op == BuilderBinaryOp::FloorDiv))
		{
			// buildSignedModDiv reads sign from `value >= 2^255`, so it needs canonical
			// 256-bit two's complement. promoteToBigUInt above ZERO-extends, so a narrower
			// signed operand (e.g. int16 -32768 -> 2^64-32768) reads as a huge POSITIVE
			// number -> wrong abs/sign (int128/int16 div returned 0). Sign-extend each from
			// its own width (idempotent for already-canonical int128/int256 operands).
			unsigned lhsBits = _reverse ? otherInt->numBits() : m_bits;
			unsigned rhsBits = _reverse ? m_bits : otherInt->numBits();
			if (lhsBits < 256)
				lhs = TypeCoercion::signExtendToUint256(std::move(lhs), lhsBits, _loc);
			if (rhsBits < 256)
				rhs = TypeCoercion::signExtendToUint256(std::move(rhs), rhsBits, _loc);
			auto result = buildSignedModDiv(std::move(lhs), std::move(rhs), _op, _loc);
			return wrap(std::move(result));
		}

		awst::BigUIntBinaryOperator bigOp = awst::BigUIntBinaryOperator::Add;
		switch (_op)
		{
		case BuilderBinaryOp::Add: bigOp = awst::BigUIntBinaryOperator::Add; break;
		case BuilderBinaryOp::Mult: bigOp = awst::BigUIntBinaryOperator::Mult; break;
		case BuilderBinaryOp::Div:
		case BuilderBinaryOp::FloorDiv: bigOp = awst::BigUIntBinaryOperator::FloorDiv; break;
		case BuilderBinaryOp::Mod: bigOp = awst::BigUIntBinaryOperator::Mod; break;
		case BuilderBinaryOp::BitOr: bigOp = awst::BigUIntBinaryOperator::BitOr; break;
		case BuilderBinaryOp::BitXor: bigOp = awst::BigUIntBinaryOperator::BitXor; break;
		case BuilderBinaryOp::BitAnd: bigOp = awst::BigUIntBinaryOperator::BitAnd; break;
		default: break;
		}
		auto e = awst::makeBigUIntBinOp(std::move(lhs), bigOp, std::move(rhs), _loc);

		std::shared_ptr<awst::Expression> result = e;

		if (m_scope.isUnchecked()
			&& (_op == BuilderBinaryOp::Add || _op == BuilderBinaryOp::Mult))
		{
			result = wrapMod256(std::move(result), _loc);
		}

		return wrap(emitOverflowCheck(std::move(result), _op, _loc));
	}

	// ── UInt64 path ──
	auto e = std::make_shared<awst::UInt64BinaryOperation>();
	e->sourceLocation = _loc;
	e->wtype = awst::WType::uint64Type();
	e->left = std::move(lhs);
	e->right = std::move(rhs);

	switch (_op)
	{
	case BuilderBinaryOp::Add: e->op = awst::UInt64BinaryOperator::Add; break;
	case BuilderBinaryOp::Sub:
	{
		// Unchecked narrow uint sub: AVM `-` panics on underflow; use (a+2^N-b)%2^N.
		if (m_scope.isUnchecked() && !m_signed && m_bits < 64)
		{
			uint64_t pow2N = uint64_t(1) << m_bits;
			auto powConst = awst::makeIntegerConstant(pow2N, _loc);

			auto aPlusPow = awst::makeUInt64BinOp(std::move(e->left), awst::UInt64BinaryOperator::Add, std::move(powConst), _loc);

			e->left = std::move(aPlusPow);
			}
		e->op = awst::UInt64BinaryOperator::Sub;
		break;
	}
	case BuilderBinaryOp::Mult: e->op = awst::UInt64BinaryOperator::Mult; break;
	case BuilderBinaryOp::Div:
	case BuilderBinaryOp::FloorDiv: e->op = awst::UInt64BinaryOperator::FloorDiv; break;
	case BuilderBinaryOp::Mod: e->op = awst::UInt64BinaryOperator::Mod; break;
	case BuilderBinaryOp::Pow:
	{
		// Unchecked uint exp: AVM `exp` is uint64-only and asserts on overflow; both a sub-uint64
		// intermediate (uint8 2**256) AND a full uint64 base whose power overflows 2^64 (uint64
		// MAX**2, found by the generative cast fuzzer) would revert where Solidity wraps. Route
		// through biguint square-and-multiply then mod 2**m_bits. Add/Mult/Sub at uint64 already wrap
		// (needsBigUInt / backend); exp is the one that fell in the m_bits<64 gap (== the uint64-sub gap).
		if (m_scope.isUnchecked() && !m_signed && m_bits <= 64)
		{
			auto biguintResult = buildBigUIntExp(m_ctx, m_scope.isUnchecked(), e->left, e->right, _loc);

			// 2^m_bits; uint64_t(1)<<64 is UB, so the full-uint64 modulus is spelled out.
			std::string modValStr = (m_bits == 64)
				? "18446744073709551616"
				: std::to_string(uint64_t(1) << m_bits);
			auto modConst = awst::makeIntegerConstant(modValStr, _loc, awst::WType::biguintType());
			auto masked = awst::makeBigUIntBinOp(std::move(biguintResult),
				awst::BigUIntBinaryOperator::Mod, std::move(modConst), _loc);
			auto asBytes = awst::makeAsBytes(std::move(masked), _loc);
			auto leftPadded = awst::makeLeftPad(std::move(asBytes), 8, _loc);
			auto sub8 = awst::makeUInt64BinOp(
				awst::makeLen(leftPadded, _loc),
				awst::UInt64BinaryOperator::Sub,
				awst::makeIntegerConstant("8", _loc), _loc);
			auto last8 = awst::makeExtract3(leftPadded, std::move(sub8),
				awst::makeIntegerConstant("8", _loc), _loc);
			auto u64 = awst::makeBtoi(std::move(last8), _loc);
			return wrap(std::move(u64));
		}

		// AVM `exp` asserts on 0^0; Solidity defines 0**0=1.
		e->op = awst::UInt64BinaryOperator::Pow;

		auto zero = awst::makeZero(_loc);

		auto cond = awst::makeNumericCompare(e->right, awst::NumericComparison::Eq, std::move(zero), _loc);

		auto one = awst::makeOne(_loc);

		std::shared_ptr<awst::Expression> powResult = awst::makeConditional(
			std::move(cond), std::move(one), e, awst::WType::uint64Type(), _loc);

		return wrap(emitOverflowCheck(std::move(powResult), _op, _loc));
	}
	case BuilderBinaryOp::LShift: e->op = awst::UInt64BinaryOperator::LShift; break;
	case BuilderBinaryOp::RShift: e->op = awst::UInt64BinaryOperator::RShift; break;
	case BuilderBinaryOp::BitOr: e->op = awst::UInt64BinaryOperator::BitOr; break;
	case BuilderBinaryOp::BitXor: e->op = awst::UInt64BinaryOperator::BitXor; break;
	case BuilderBinaryOp::BitAnd: e->op = awst::UInt64BinaryOperator::BitAnd; break;
	}

	std::shared_ptr<awst::Expression> result = e;

	if (m_scope.isUnchecked() && !m_signed && m_bits < 64)
	{
		bool needsWrap = (_op == BuilderBinaryOp::Add || _op == BuilderBinaryOp::Sub
			|| _op == BuilderBinaryOp::Mult || _op == BuilderBinaryOp::Pow);
		if (needsWrap)
		{
			uint64_t modVal = uint64_t(1) << m_bits;
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
	bool needsBigUInt = m_isBigUInt || otherIsBigUInt;
	bool isSigned = m_signed || otherInt->isSigned();

	auto lhs = resolve();
	auto rhs = _other.resolve();

	// Operands arrive coerced to the op's commonType (same width + wtype, already
	// canonical) from SolBinaryOperation's coerceToCommonInt, so the old
	// narrowConstIfNegative const-narrowing and per-operand sign-extension here are
	// unnecessary — only the biguint promotion (cheap no-op when already biguint) and
	// the signed-ordering sign-bit XOR remain.

	if (needsBigUInt)
	{
		lhs = promoteToBigUInt(std::move(lhs), _loc);
		rhs = promoteToBigUInt(std::move(rhs), _loc);
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
			lhs = promoteToBigUInt(std::move(lhs), _loc);
		else if (rhs->wtype == awst::WType::uint64Type() && lhs->wtype == awst::WType::biguintType())
			rhs = promoteToBigUInt(std::move(rhs), _loc);
	}

	awst::NumericComparison cmpOp = awst::NumericComparison::Eq;
	switch (_op)
	{
	case BuilderComparisonOp::Eq: cmpOp = awst::NumericComparison::Eq; break;
	case BuilderComparisonOp::Ne: cmpOp = awst::NumericComparison::Ne; break;
	case BuilderComparisonOp::Lt: cmpOp = awst::NumericComparison::Lt; break;
	case BuilderComparisonOp::Lte: cmpOp = awst::NumericComparison::Lte; break;
	case BuilderComparisonOp::Gt: cmpOp = awst::NumericComparison::Gt; break;
	case BuilderComparisonOp::Gte: cmpOp = awst::NumericComparison::Gte; break;
	}
	auto cmp = awst::makeNumericCompare(std::move(lhs), cmpOp, std::move(rhs), _loc);

	// TODO: return SolBoolBuilder; for now reuse SolIntegerBuilder with m_intType (wtype is correct boolType).
	return std::make_unique<SolIntegerBuilder>(m_ctx, m_intType, std::move(cmp));
}

// ─────────────────────────────────────────────────────────────────────
// unary_op
// ─────────────────────────────────────────────────────────────────────

std::unique_ptr<InstanceBuilder> SolIntegerBuilder::unary_op(
	BuilderUnaryOp _op, awst::SourceLocation const& _loc)
{
	switch (_op)
	{
	case BuilderUnaryOp::Positive:
		return wrap(resolve());

	case BuilderUnaryOp::Negative:
	{
		auto operand = resolve();
		auto const* intConst = dynamic_cast<awst::IntegerConstant const*>(operand.get());
		if (intConst && !intConst->value.empty() && intConst->value != "0")
		{
			try
			{
				unsigned long long val = std::stoull(intConst->value);
				if (val > 0)
				{
					if (m_isBigUInt)
					{
						solidity::u256 mod256 = solidity::u256(1) << 256;
						solidity::u256 result = mod256 - solidity::u256(val);
						auto e = awst::makeIntegerConstant(result.str(), _loc, awst::WType::biguintType());
						return wrap(std::move(e));
					}
					unsigned long long result = (UINT64_MAX - val) + 1ULL;
					auto e = awst::makeIntegerConstant(result, _loc);
					return wrap(std::move(e));
				}
			}
			catch (...) {} // fall through
		}
		// Checked signed: -INT_MIN overflows. INT_MIN's TC pattern is 2^(N-1) in the
		// low N bits; as a 256-bit sign-extended value it reads as 2^256 - 2^(N-1).
		if (m_signed && !m_scope.isUnchecked())
		{
			solidity::u256 minLowBits = solidity::u256(1) << (m_bits - 1);   // 2^(N-1)

			std::shared_ptr<awst::Expression> cmpOperand = operand;
			solidity::u256 cmpAgainst;
			if (!m_isBigUInt)
			{
				// uint64-backed: mask to the low N bits (the slot may hold a wider TC)
				// and compare against 2^(N-1). (1<<64)-1 is UB, so all-ones for N==64.
				uint64_t mask = m_bits >= 64 ? UINT64_MAX : ((uint64_t(1) << m_bits) - 1);
				auto maskConst = awst::makeIntegerConstant(mask, _loc);

				auto masked = awst::makeUInt64BinOp(operand, awst::UInt64BinaryOperator::BitAnd, std::move(maskConst), _loc);

				auto itob = awst::makeItob(std::move(masked), _loc);
				cmpOperand = awst::makeAsBiguint(std::move(itob), _loc);
				cmpAgainst = minLowBits;
			}
			else
			{
				// biguint-backed: operand is the 256-bit sign-extended TC, so INT_MIN
				// reads as 2^256 - 2^(N-1) (for N==256 that is 2^255).
				cmpAgainst = (solidity::u256(1) << 256) - minLowBits;
			}

			std::ostringstream oss;
			oss << cmpAgainst;
			auto halfConst = awst::makeIntegerConstant(oss.str(), _loc, awst::WType::biguintType());

			auto cmp = awst::makeNumericCompare(std::move(cmpOperand), awst::NumericComparison::Ne, std::move(halfConst), _loc);

			auto assertStmt = awst::makeExpressionStatement(awst::makeAssert(std::move(cmp), _loc, "signed negation overflow"), _loc);
			m_ctx.prePendingStatements.push_back(std::move(assertStmt));
		}

		if (m_isBigUInt)
		{
			// -x = ~x+1. AVM `b~` inverts only actual bytes (5→0x05 inverts to 0xFA not 256-bit
			// complement); pad to 32 first so `~` produces the full 32-byte result.
			auto castToBytes = awst::makeAsBytes(std::move(operand), _loc);

			auto concatPad = awst::makeLeftPad(std::move(castToBytes), 32, _loc);

			auto lenCall = awst::makeLen(concatPad, _loc);

			auto startOff = awst::makeIntrinsicCall("-", awst::WType::uint64Type(), _loc);
			startOff->stackArgs.push_back(std::move(lenCall));
			startOff->stackArgs.push_back(awst::makeIntegerConstant("32", _loc));

			auto extract = awst::makeExtract3(concatPad, std::move(startOff), awst::makeIntegerConstant("32", _loc), _loc);
			auto bitInvert = awst::makeBitInvert(std::move(extract), awst::WType::bytesType(), _loc);

			auto castBack = awst::makeAsBiguint(std::move(bitInvert), _loc);

			auto one = awst::makeBiguintConstant("1", _loc);

			auto addOne = awst::makeBigUIntBinOp(std::move(castBack), awst::BigUIntBinaryOperator::Add, std::move(one), _loc);

			// Mod 2^256: handles -0 overflow (2^256 wraps to 0).
			auto modConst = makePow256(_loc);

			std::shared_ptr<awst::Expression> wrapped =
				awst::makeBigUIntBinOp(std::move(addOne), awst::BigUIntBinaryOperator::Mod, std::move(modConst), _loc);

			// Sub-word signed (64<N<256): same -INT_MIN overflow as the uint64 path — wrap to N bits
			// + sign-extend to canonical 256-bit TC so it composes (e.g. int128 `(-a) > a` at INT128_MIN).
			if (m_signed && m_bits < 256)
			{
				solidity::u256 modN = solidity::u256(1) << m_bits;
				auto modN_c = awst::makeBiguintConstant(modN.str(), _loc);
				auto masked = awst::makeBigUIntBinOp(std::move(wrapped), awst::BigUIntBinaryOperator::Mod, std::move(modN_c), _loc);
				wrapped = TypeCoercion::signExtendToUint256(std::move(masked), m_bits, _loc);
			}
			return wrap(std::move(wrapped));
		}
		// uint64: -x via (2^64 - x) % 2^64 (0 - x underflows in uint64).
		{
			auto itob = awst::makeItob(std::move(operand), _loc);
			auto castBiguint = awst::makeAsBiguint(std::move(itob), _loc);
			auto pow2_64 = awst::makeBiguintConstant("18446744073709551616", _loc); // 2^64
			auto sub = awst::makeBigUIntBinOp(std::move(pow2_64), awst::BigUIntBinaryOperator::Sub, std::move(castBiguint), _loc);
			auto pow2_64_2 = awst::makeBiguintConstant("18446744073709551616", _loc);
			auto mod = awst::makeBigUIntBinOp(std::move(sub), awst::BigUIntBinaryOperator::Mod, std::move(pow2_64_2), _loc);
			auto result = TypeCoercion::implicitNumericCast(std::move(mod), awst::WType::uint64Type(), _loc);
			// Sub-word signed: -INT_MIN computes to +2^(N-1), which overflows the N-bit range. Wrap
			// to N bits + sign-extend so it reads as INT_MIN canonically when used as a subexpression
			// (e.g. `(-a) > a`); the return/ABI path re-truncates, so a bare `-a` was already right.
			if (m_signed && m_bits < 64)
			{
				auto maskC = awst::makeIntegerConstant((uint64_t(1) << m_bits) - 1, _loc);
				auto masked = awst::makeUInt64BinOp(std::move(result), awst::UInt64BinaryOperator::BitAnd, std::move(maskC), _loc);
				result = TypeCoercion::signExtendToUint64(std::move(masked), m_bits, _loc);
			}
			return wrap(std::move(result));
		}
	}

	case BuilderUnaryOp::BitInvert:
	{
		if (m_isBigUInt)
		{
			// ~x for biguint: minimal encoding (e.g. 1<<65 = 9 bytes); ~9 = 9 bytes;
			// ANDed with 32-byte word clears high 23 bytes' bits — corrupts mask.
			// Pad to 32 first: extract last 32 bytes of bzero(32)||biguint.
			auto toBytes = awst::makeAsBytes(resolve(), _loc);

			auto cat = awst::makeLeftPad(std::move(toBytes), 32, _loc);
			auto lenCall = awst::makeLen(cat, _loc);
			auto thirtyTwo = awst::makeIntegerConstant("32", _loc);
			auto offset = awst::makeIntrinsicCall(
				"-", awst::WType::uint64Type(), _loc);
			offset->stackArgs.push_back(std::move(lenCall));
			offset->stackArgs.push_back(thirtyTwo);
			auto thirtyTwo2 = awst::makeIntegerConstant("32", _loc);
			auto extract = awst::makeIntrinsicCall(
				"extract3", awst::WType::bytesType(), _loc);
			extract->stackArgs.push_back(std::move(cat));
			extract->stackArgs.push_back(std::move(offset));
			extract->stackArgs.push_back(std::move(thirtyTwo2));

			auto invert = awst::makeBitInvert(std::move(extract), awst::WType::bytesType(), _loc);

			auto cast = awst::makeAsBiguint(std::move(invert), _loc);
			return wrap(std::move(cast));
		}
		{
			solidity::u256 mask = (m_bits >= 64)
				? solidity::u256("18446744073709551615")
				: (solidity::u256(1) << m_bits) - 1;
			std::ostringstream oss;
			oss << mask;

			auto maxVal = awst::makeIntegerConstant(oss.str(), _loc);

			auto e = awst::makeUInt64BinOp(resolve(), awst::UInt64BinaryOperator::BitXor, std::move(maxVal), _loc);
			return wrap(std::move(e));
		}
	}

	default:
		return nullptr; // inc/dec handled by visitor
	}
}

// ─────────────────────────────────────────────────────────────────────
// bool_eval
// ─────────────────────────────────────────────────────────────────────

std::unique_ptr<InstanceBuilder> SolIntegerBuilder::bool_eval(
	awst::SourceLocation const& _loc, bool _negate)
{
	auto zero = awst::makeZero(_loc, m_isBigUInt ? awst::WType::biguintType() : awst::WType::uint64Type());

	auto cmp = awst::makeNumericCompare(resolve(), _negate ? awst::NumericComparison::Eq : awst::NumericComparison::Ne, std::move(zero), _loc);

	return wrap(std::move(cmp));
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
	if (!needsCheck || m_signed)
		return _result;

	unsigned maxBits = m_isBigUInt ? 256 : 64;
	// uint64 (native): the AVM +/*/exp opcodes revert on overflow themselves, so no explicit check
	// at the max width. BigUInt (uint65..uint256): does NOT auto-revert at 2^bits — biguint is
	// arbitrary precision (up to the AVM 512-bit cap), so the result of `s+1` at uint256 is the
	// exact 2^256, not a wrapped 0. It MUST be checked at every width INCLUDING 256; otherwise
	// `uint64(s + 1)` truncates that 2^256 to 0 before any downstream (return/store) check sees it,
	// silently wrapping instead of reverting. Found by the differential fuzzer.
	if (m_bits >= maxBits && !m_isBigUInt)
		return _result;

	static int checkedCounter = 0;
	std::string tmpName = "__checked_" + std::to_string(checkedCounter++);
	auto* resType = _result->wtype;

	std::string maxValStr;
	if (m_isBigUInt)
		maxValStr = ((solidity::u256(1) << m_bits) - 1).str();
	else
		maxValStr = std::to_string((uint64_t(1) << m_bits) - 1);

	auto mkCmp = [&]() {
		return awst::makeNumericCompare(
			awst::makeVarExpression(tmpName, resType, _loc), awst::NumericComparison::Lte,
			awst::makeIntegerConstant(std::string(maxValStr), _loc, resType), _loc);
	};

	// uint256 (m_bits==256): emit the check INLINE as a comma expression, not as pre-statements.
	// uint256 ops first reach emitOverflowCheck in modifier-arg / constructor / return-expression
	// contexts that don't flush prePendingStatements at the right point, so a pre-statement check
	// is mis-placed there (regressed g()'s `r+r` modifier args etc.). A comma `(t=res, assert, t)`
	// is a pure value expression and composes anywhere. (Sub-256 keeps the existing pre-stmt form.)
	if (m_isBigUInt && m_bits == 256)
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
	m_ctx.prePendingStatements.push_back(std::move(assign));
	auto assertStmt = awst::makeExpressionStatement(awst::makeAssert(mkCmp(), _loc, "overflow"), _loc);
	m_ctx.prePendingStatements.push_back(std::move(assertStmt));
	return tmpVar;
}

} // namespace puyasol::builder::eb
