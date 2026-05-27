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

	// For integer constants, produce a biguint constant directly
	if (auto const* intConst = dynamic_cast<awst::IntegerConstant const*>(_expr.get()))
	{
		auto bigConst = awst::makeIntegerConstant(intConst->value, _loc, awst::WType::biguintType());
		return bigConst;
	}

	// account / fixed-bytes / dynamic bytes are already big-endian byte
	// buffers — reinterpret-cast to biguint directly. Going through itob
	// (which is uint64-only) would silently truncate everything above the
	// low 8 bytes. Mirrors the parallel coercion fix in
	// AssemblyBuilder::ensureBiguint (commit `01332f363`).
	if (_expr->wtype == awst::WType::accountType()
		|| (_expr->wtype && _expr->wtype->kind() == awst::WTypeKind::Bytes))
	{
		auto bytesExpr = _expr->wtype == awst::WType::accountType()
			? awst::makeAsBytes(std::move(_expr), _loc)
			: std::move(_expr);
		return awst::makeAsBiguint(std::move(bytesExpr), _loc);
	}

	// uint64 → biguint via itob → ReinterpretCast (canonical path)
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
	// Only handle integer × integer
	auto const* otherInt = dynamic_cast<solidity::frontend::IntegerType const*>(_other.solType());
	if (!otherInt)
		return nullptr;

	bool otherIsBigUInt = otherInt->numBits() > 64;
	bool needsBigUInt = m_isBigUInt || otherIsBigUInt;

	auto lhs = resolve();
	auto rhs = _other.resolve();
	if (_reverse)
		std::swap(lhs, rhs);

	// ── BigUInt path ──
	if (needsBigUInt)
	{
		lhs = promoteToBigUInt(std::move(lhs), _loc);

		// Shifts: don't promote the shift amount — it stays uint64
		if (_op == BuilderBinaryOp::LShift || _op == BuilderBinaryOp::RShift)
		{
			auto shiftAmt = TypeCoercion::implicitNumericCast(
				std::move(rhs), awst::WType::uint64Type(), _loc);
			auto result = buildBigUIntShift(std::move(lhs), std::move(shiftAmt),
				_op == BuilderBinaryOp::LShift, _loc);
			return wrap(emitOverflowCheck(std::move(result), _op, _loc));
		}

		rhs = promoteToBigUInt(std::move(rhs), _loc);

		// Subtraction: wrapping (a + 2^256 - b) % 2^256
		if (_op == BuilderBinaryOp::Sub)
		{
			auto result = buildWrappingSubtract(m_ctx, m_scope.isUnchecked(), std::move(lhs), std::move(rhs), _loc);
			return wrap(emitOverflowCheck(std::move(result), _op, _loc));
		}

		// Exponentiation: square-and-multiply loop
		if (_op == BuilderBinaryOp::Pow)
		{
			auto result = buildBigUIntExp(m_ctx, m_scope.isUnchecked(), std::move(lhs), std::move(rhs), _loc);
			return wrap(emitOverflowCheck(std::move(result), _op, _loc));
		}

		// Signed mod/div: operate on absolute values, then apply sign
		if (m_signed && (_op == BuilderBinaryOp::Mod || _op == BuilderBinaryOp::FloorDiv))
		{
			auto result = buildSignedModDiv(std::move(lhs), std::move(rhs), _op, _loc);
			return wrap(std::move(result));
		}

		// Standard biguint arithmetic
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

		// Unchecked wrapping mod 2^256
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
		// Unchecked uint sub for narrow types: AVM `-` panics on underflow,
		// so use (a + 2^N - b) % 2^N instead to wrap correctly.
		if (m_scope.isUnchecked() && !m_signed && m_bits < 64)
		{
			uint64_t pow2N = uint64_t(1) << m_bits;
			auto powConst = awst::makeIntegerConstant(pow2N, _loc);

			auto aPlusPow = awst::makeUInt64BinOp(std::move(e->left), awst::UInt64BinaryOperator::Add, std::move(powConst), _loc);

			e->left = std::move(aPlusPow);
			// e->right stays the same: (a + 2^N) - b
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
		// For unchecked sub-uint64 types, the intermediate `base**exp`
		// value can exceed uint64 (e.g. `uint16 e = 0x100; uint8 b = 2;
		// b ** e` = 2**256, then mod 2**16 = 0). AVM's `exp` opcode is
		// uint64-only and asserts on overflow, so we can't compute the
		// intermediate there and then mod down. Route through biguint
		// `exp` (square-and-multiply, no overflow) then mod 2**m_bits.
		// (Was: AVM uint64 `exp` unconditionally; under puya 5.9's
		// stricter optimizer this surfaced as a `2**256 overflows uint64`
		// runtime revert in test_exp_cleanup_smaller_base —
		// puyabug.md #4b.)
		if (m_scope.isUnchecked() && !m_signed && m_bits < 64)
		{
			auto biguintResult = buildBigUIntExp(m_ctx, m_scope.isUnchecked(), e->left, e->right, _loc);

			// Mask to 2**m_bits and cast back to uint64.
			std::string modValStr;
			{
				uint64_t modVal = uint64_t(1) << m_bits;
				modValStr = std::to_string(modVal);
			}
			auto modConst = awst::makeIntegerConstant(modValStr, _loc, awst::WType::biguintType());
			auto masked = awst::makeBigUIntBinOp(std::move(biguintResult),
				awst::BigUIntBinaryOperator::Mod, std::move(modConst), _loc);
			// biguint → bytes → uint64 (low 8 bytes).
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

		// AVM `exp` asserts on 0^0. Solidity defines 0**0 = 1.
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

	// Unchecked uint64 narrow wrapping: mask to Solidity bit width
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

	// Width-mismatch sign extension (must run BEFORE promotion / signed XOR):
	// when comparing a uint64 (small int slot) against a biguint constant
	// whose magnitude doesn't fit in uint64 (val > 2^63), the biguint side
	// is the 256-bit two's complement of a "negative" small int (e.g.
	// -128 → biguint(2^256 - 128)). Naive promotion of the uint64 side
	// via itob would give biguint(2^64 - 128), which doesn't match the
	// 32-byte encoding. Instead narrow the biguint constant to uint64 by
	// modular reduction so both sides line up in the small int slot's
	// 64-bit two's complement form.
	auto narrowConstIfNegative = [&](std::shared_ptr<awst::Expression>& wide,
		std::shared_ptr<awst::Expression> const& other)
	{
		if (other->wtype != awst::WType::uint64Type()) return;
		if (wide->wtype != awst::WType::biguintType()) return;
		auto const* intConst = dynamic_cast<awst::IntegerConstant const*>(wide.get());
		if (!intConst) return;
		try
		{
			solidity::u256 val(intConst->value);
			static const solidity::u256 twoPow63("9223372036854775808");
			static const solidity::u256 twoPow64("18446744073709551616");
			if (val < twoPow63) return;
			solidity::u256 wrapped = val % twoPow64;
			auto e = awst::makeIntegerConstant(wrapped.str(), _loc);
			wide = std::move(e);
		}
		catch (...) {}
	};
	if (!needsBigUInt && lhs->wtype != rhs->wtype)
	{
		narrowConstIfNegative(lhs, rhs);
		narrowConstIfNegative(rhs, lhs);
	}

	// Promote if mixed uint64/biguint
	if (needsBigUInt)
	{
		lhs = promoteToBigUInt(std::move(lhs), _loc);
		rhs = promoteToBigUInt(std::move(rhs), _loc);
	}

	// Signed ordering comparisons: XOR with sign bit to convert signed→unsigned ordering
	bool isOrderingOp = (_op == BuilderComparisonOp::Lt || _op == BuilderComparisonOp::Lte
		|| _op == BuilderComparisonOp::Gt || _op == BuilderComparisonOp::Gte);

	if (isSigned && isOrderingOp)
	{
		if (needsBigUInt)
		{
			// XOR with 2^255 for biguint
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
			// XOR with 2^63 for uint64
			auto signBit = awst::makeIntegerConstant("9223372036854775808", _loc); // 2^63

			auto xorL = awst::makeUInt64BinOp(std::move(lhs), awst::UInt64BinaryOperator::BitXor, signBit, _loc);
			lhs = std::move(xorL);

			auto signBit2 = awst::makeIntegerConstant("9223372036854775808", _loc);

			auto xorR = awst::makeUInt64BinOp(std::move(rhs), awst::UInt64BinaryOperator::BitXor, std::move(signBit2), _loc);
			rhs = std::move(xorR);
		}
	}

	// Ensure both sides have matching types for the comparison.
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

	// Comparison returns a bool — we can't return a SolIntegerBuilder.
	// For now, return a generic InstanceBuilder. When SolBoolBuilder exists,
	// return that instead.
	return std::make_unique<SolIntegerBuilder>(
		m_ctx,
		// bool result — use a dummy IntegerType. This is a temporary hack
		// until SolBoolBuilder exists. The wtype is correct (boolType) from the expression.
		m_intType, // TODO: replace with proper bool builder in Phase 2
		std::move(cmp));
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
		// Constant folding: two's complement
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
		// Signed overflow check: -INT_MIN overflows
		if (m_signed && !m_scope.isUnchecked())
		{
			// INT_MIN = 2^(N-1) in two's complement unsigned representation
			std::string halfNStr;
			if (m_bits == 256)
				halfNStr = "57896044618658097711785492504343953926634992332820282019728792003956564819968";
			else
			{
				solidity::u256 halfN = solidity::u256(1) << (m_bits - 1);
				std::ostringstream oss;
				oss << halfN;
				halfNStr = oss.str();
			}

			// Promote operand to biguint for comparison if needed
			std::shared_ptr<awst::Expression> cmpOperand = operand;
			if (!m_isBigUInt)
			{
				// Mask to N bits first (uint64 may hold wider two's complement)
				auto maskConst = awst::makeIntegerConstant((uint64_t(1) << m_bits) - 1, _loc);

				auto masked = awst::makeUInt64BinOp(operand, awst::UInt64BinaryOperator::BitAnd, std::move(maskConst), _loc);

				// Promote to biguint for comparison
				auto itob = awst::makeItob(std::move(masked), _loc);
				cmpOperand = awst::makeAsBiguint(std::move(itob), _loc);
			}

			auto halfConst = awst::makeIntegerConstant(halfNStr, _loc, awst::WType::biguintType());

			auto cmp = awst::makeNumericCompare(std::move(cmpOperand), awst::NumericComparison::Ne, std::move(halfConst), _loc);

			auto assertStmt = awst::makeExpressionStatement(awst::makeAssert(std::move(cmp), _loc, "signed negation overflow"), _loc);
			m_ctx.prePendingStatements.push_back(std::move(assertStmt));
		}

		// Runtime negation
		if (m_isBigUInt)
		{
			// Two's complement: -x = ~x + 1
			// AVM `b~` inverts actual bytes; a minimal biguint encoding like 5→0x05 would
			// invert to 0xFA (250) instead of the full 256-bit complement. Pad to 32 bytes
			// first so `~` produces a 256-bit result.
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

			// Mod 2^256 to handle -0 overflow (2^256 wraps to 0)
			auto modConst = makePow256(_loc);

			auto wrapped = awst::makeBigUIntBinOp(std::move(addOne), awst::BigUIntBinaryOperator::Mod, std::move(modConst), _loc);

			return wrap(std::move(wrapped));
		}
		// uint64: two's complement negation via 2^64 - operand
		// (0 - operand would underflow in uint64 for positive operands)
		{
			// Promote to biguint: itob → ReinterpretCast
			auto itob = awst::makeItob(std::move(operand), _loc);

			auto castBiguint = awst::makeAsBiguint(std::move(itob), _loc);

			// 2^64 - x
			auto pow2_64 = awst::makeBiguintConstant("18446744073709551616", _loc); // 2^64

			auto sub = awst::makeBigUIntBinOp(std::move(pow2_64), awst::BigUIntBinaryOperator::Sub, std::move(castBiguint), _loc);

			// mod 2^64 to wrap
			auto pow2_64_2 = awst::makeBiguintConstant("18446744073709551616", _loc);

			auto mod = awst::makeBigUIntBinOp(std::move(sub), awst::BigUIntBinaryOperator::Mod, std::move(pow2_64_2), _loc);

			// Back to uint64: safe extract
			auto result = TypeCoercion::implicitNumericCast(std::move(mod), awst::WType::uint64Type(), _loc);
			return wrap(std::move(result));
		}
	}

	case BuilderUnaryOp::BitInvert:
	{
		if (m_isBigUInt)
		{
			// ~x for biguint: pad bytes to 32 before BitInvert.
			// Without padding, a biguint like `1 << 65` encodes
			// minimally as 9 bytes; ~9-byte = 9-byte; ANDing that
			// with a 32-byte map word right-aligns and silently
			// clears the high 23 bytes' bits — corrupts mask.
			// Pattern: extract last 32 bytes of `bzero(32) || biguint`.
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
		// ~x for uint64: XOR with bit-width mask (not always 2^64-1)
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
		return nullptr; // inc/dec/negative handled by visitor for now
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

	// Returns a bool-typed expression wrapped in an integer builder (temporary)
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
	if (m_bits >= maxBits)
		return _result;

	// Narrow type: emit assert(result <= max)
	static int checkedCounter = 0;
	std::string tmpName = "__checked_" + std::to_string(checkedCounter++);
	auto* resType = _result->wtype;

	auto tmpVar = awst::makeVarExpression(tmpName, resType, _loc);

	auto assign = awst::makeAssignmentStatement(tmpVar, std::move(_result), _loc);
	m_ctx.prePendingStatements.push_back(std::move(assign));

	std::string maxValStr;
	if (m_isBigUInt)
		maxValStr = ((solidity::u256(1) << m_bits) - 1).str();
	else
		maxValStr = std::to_string((uint64_t(1) << m_bits) - 1);
	auto maxConst = awst::makeIntegerConstant(std::move(maxValStr), _loc, resType);

	auto cmp = awst::makeNumericCompare(tmpVar, awst::NumericComparison::Lte, std::move(maxConst), _loc);

	auto assertStmt = awst::makeExpressionStatement(awst::makeAssert(std::move(cmp), _loc, "overflow"), _loc);
	m_ctx.prePendingStatements.push_back(std::move(assertStmt));

	return tmpVar;
}

} // namespace puyasol::builder::eb
