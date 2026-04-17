/// @file SolIntegerBuilder.cpp
/// Solidity integer type builder — handles all int/uint operations with full
/// Solidity semantics including overflow checking, signed comparison, wrapping.

#include "builder/sol-eb/SolIntegerBuilder.h"
#include "builder/sol-types/TypeCoercion.h"
#include "Logger.h"

#include <libsolidity/ast/TypeProvider.h>
#include <libsolutil/Numeric.h>
#include <sstream>

namespace puyasol::builder::eb
{

SolIntegerBuilder::SolIntegerBuilder(
	BuilderContext& _ctx,
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

	// itob → ReinterpretCast to biguint
	auto itob = awst::makeIntrinsicCall("itob", awst::WType::bytesType(), _loc);
	itob->stackArgs.push_back(std::move(_expr));

	auto cast = awst::makeReinterpretCast(std::move(itob), awst::WType::biguintType(), _loc);
	return cast;
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
			auto result = buildWrappingSubtract(std::move(lhs), std::move(rhs), _loc);
			return wrap(emitOverflowCheck(std::move(result), _op, _loc));
		}

		// Exponentiation: square-and-multiply loop
		if (_op == BuilderBinaryOp::Pow)
		{
			auto result = buildBigUIntExp(std::move(lhs), std::move(rhs), _loc);
			return wrap(emitOverflowCheck(std::move(result), _op, _loc));
		}

		// Signed mod/div: operate on absolute values, then apply sign
		if (m_signed && (_op == BuilderBinaryOp::Mod || _op == BuilderBinaryOp::FloorDiv))
		{
			auto result = buildSignedModDiv(std::move(lhs), std::move(rhs), _op, _loc);
			return wrap(std::move(result));
		}

		// Standard biguint arithmetic
		auto e = std::make_shared<awst::BigUIntBinaryOperation>();
		e->sourceLocation = _loc;
		e->wtype = awst::WType::biguintType();
		e->left = std::move(lhs);
		e->right = std::move(rhs);

		switch (_op)
		{
		case BuilderBinaryOp::Add: e->op = awst::BigUIntBinaryOperator::Add; break;
		case BuilderBinaryOp::Mult: e->op = awst::BigUIntBinaryOperator::Mult; break;
		case BuilderBinaryOp::Div:
		case BuilderBinaryOp::FloorDiv: e->op = awst::BigUIntBinaryOperator::FloorDiv; break;
		case BuilderBinaryOp::Mod: e->op = awst::BigUIntBinaryOperator::Mod; break;
		case BuilderBinaryOp::BitOr: e->op = awst::BigUIntBinaryOperator::BitOr; break;
		case BuilderBinaryOp::BitXor: e->op = awst::BigUIntBinaryOperator::BitXor; break;
		case BuilderBinaryOp::BitAnd: e->op = awst::BigUIntBinaryOperator::BitAnd; break;
		default: e->op = awst::BigUIntBinaryOperator::Add; break;
		}

		std::shared_ptr<awst::Expression> result = e;

		// Unchecked wrapping mod 2^256
		if (m_ctx.inUncheckedBlock
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
		if (m_ctx.inUncheckedBlock && !m_signed && m_bits < 64)
		{
			uint64_t pow2N = uint64_t(1) << m_bits;
			auto powConst = awst::makeIntegerConstant(std::to_string(pow2N), _loc);

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
		// AVM `exp` asserts on 0^0. Solidity defines 0**0 = 1.
		e->op = awst::UInt64BinaryOperator::Pow;

		auto zero = awst::makeIntegerConstant("0", _loc);

		auto cond = awst::makeNumericCompare(e->right, awst::NumericComparison::Eq, std::move(zero), _loc);

		auto one = awst::makeIntegerConstant("1", _loc);

		auto ternary = std::make_shared<awst::ConditionalExpression>();
		ternary->sourceLocation = _loc;
		ternary->wtype = awst::WType::uint64Type();
		ternary->condition = std::move(cond);
		ternary->trueExpr = std::move(one);
		ternary->falseExpr = e;
		std::shared_ptr<awst::Expression> powResult = std::move(ternary);

		// Apply unchecked sub-type wrapping for Pow (can't fall through to general wrapping)
		if (m_ctx.inUncheckedBlock && !m_signed && m_bits < 64)
		{
			uint64_t modVal = uint64_t(1) << m_bits;
			auto modConst = awst::makeIntegerConstant(std::to_string(modVal), _loc);
			auto masked = awst::makeUInt64BinOp(std::move(powResult), awst::UInt64BinaryOperator::Mod, std::move(modConst), _loc);
			powResult = std::move(masked);
		}

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
	if (m_ctx.inUncheckedBlock && !m_signed && m_bits < 64)
	{
		bool needsWrap = (_op == BuilderBinaryOp::Add || _op == BuilderBinaryOp::Sub
			|| _op == BuilderBinaryOp::Mult || _op == BuilderBinaryOp::Pow);
		if (needsWrap)
		{
			uint64_t modVal = uint64_t(1) << m_bits;
			auto modConst = awst::makeIntegerConstant(std::to_string(modVal), _loc);

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

			auto xorL = std::make_shared<awst::BigUIntBinaryOperation>();
			xorL->sourceLocation = _loc;
			xorL->wtype = awst::WType::biguintType();
			xorL->left = std::move(lhs);
			xorL->op = awst::BigUIntBinaryOperator::BitXor;
			xorL->right = signBit;
			lhs = std::move(xorL);

			auto signBit2 = awst::makeIntegerConstant(signBitVal.str(), _loc, awst::WType::biguintType());

			auto xorR = std::make_shared<awst::BigUIntBinaryOperation>();
			xorR->sourceLocation = _loc;
			xorR->wtype = awst::WType::biguintType();
			xorR->left = std::move(rhs);
			xorR->op = awst::BigUIntBinaryOperator::BitXor;
			xorR->right = std::move(signBit2);
			rhs = std::move(xorR);
		}
		else
		{
			// XOR with 2^63 for uint64
			auto signBit = std::make_shared<awst::IntegerConstant>();
			signBit->sourceLocation = _loc;
			signBit->wtype = awst::WType::uint64Type();
			signBit->value = "9223372036854775808"; // 2^63

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

	auto cmp = std::make_shared<awst::NumericComparisonExpression>();
	cmp->sourceLocation = _loc;
	cmp->wtype = awst::WType::boolType();
	cmp->lhs = std::move(lhs);
	cmp->rhs = std::move(rhs);

	switch (_op)
	{
	case BuilderComparisonOp::Eq: cmp->op = awst::NumericComparison::Eq; break;
	case BuilderComparisonOp::Ne: cmp->op = awst::NumericComparison::Ne; break;
	case BuilderComparisonOp::Lt: cmp->op = awst::NumericComparison::Lt; break;
	case BuilderComparisonOp::Lte: cmp->op = awst::NumericComparison::Lte; break;
	case BuilderComparisonOp::Gt: cmp->op = awst::NumericComparison::Gt; break;
	case BuilderComparisonOp::Gte: cmp->op = awst::NumericComparison::Gte; break;
	}

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
					auto e = awst::makeIntegerConstant(std::to_string(result), _loc);
					return wrap(std::move(e));
				}
			}
			catch (...) {} // fall through
		}
		// Signed overflow check: -INT_MIN overflows
		if (m_signed && !m_ctx.inUncheckedBlock)
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
				auto maskConst = awst::makeIntegerConstant(std::to_string((uint64_t(1) << m_bits) - 1), _loc);

				auto masked = awst::makeUInt64BinOp(operand, awst::UInt64BinaryOperator::BitAnd, std::move(maskConst), _loc);

				// Promote to biguint for comparison
				auto itob = awst::makeIntrinsicCall("itob", awst::WType::bytesType(), _loc);
				itob->stackArgs.push_back(std::move(masked));

				auto cast = awst::makeReinterpretCast(std::move(itob), awst::WType::biguintType(), _loc);
				cmpOperand = std::move(cast);
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
			// Guard: ~0 + 1 = 2^256 overflows 256-bit biguint, so mask with % 2^256
			auto castToBytes = awst::makeReinterpretCast(std::move(operand), awst::WType::bytesType(), _loc);

			auto bitInvert = std::make_shared<awst::BytesUnaryOperation>();
			bitInvert->sourceLocation = _loc;
			bitInvert->wtype = awst::WType::bytesType();
			bitInvert->op = awst::BytesUnaryOperator::BitInvert;
			bitInvert->expr = std::move(castToBytes);

			auto castBack = awst::makeReinterpretCast(std::move(bitInvert), awst::WType::biguintType(), _loc);

			auto one = awst::makeIntegerConstant("1", _loc, awst::WType::biguintType());

			auto addOne = std::make_shared<awst::BigUIntBinaryOperation>();
			addOne->sourceLocation = _loc;
			addOne->wtype = awst::WType::biguintType();
			addOne->left = std::move(castBack);
			addOne->op = awst::BigUIntBinaryOperator::Add;
			addOne->right = std::move(one);

			// Mod 2^256 to handle -0 overflow (2^256 wraps to 0)
			auto modConst = awst::makeIntegerConstant(kPow2_256, _loc, awst::WType::biguintType());

			auto wrapped = std::make_shared<awst::BigUIntBinaryOperation>();
			wrapped->sourceLocation = _loc;
			wrapped->wtype = awst::WType::biguintType();
			wrapped->left = std::move(addOne);
			wrapped->op = awst::BigUIntBinaryOperator::Mod;
			wrapped->right = std::move(modConst);

			return wrap(std::move(wrapped));
		}
		// uint64: two's complement negation via 2^64 - operand
		// (0 - operand would underflow in uint64 for positive operands)
		{
			// Promote to biguint: itob → ReinterpretCast
			auto itob = awst::makeIntrinsicCall("itob", awst::WType::bytesType(), _loc);
			itob->stackArgs.push_back(std::move(operand));

			auto castBiguint = awst::makeReinterpretCast(std::move(itob), awst::WType::biguintType(), _loc);

			// 2^64 - x
			auto pow2_64 = std::make_shared<awst::IntegerConstant>();
			pow2_64->sourceLocation = _loc;
			pow2_64->wtype = awst::WType::biguintType();
			pow2_64->value = "18446744073709551616"; // 2^64

			auto sub = std::make_shared<awst::BigUIntBinaryOperation>();
			sub->sourceLocation = _loc;
			sub->wtype = awst::WType::biguintType();
			sub->left = std::move(pow2_64);
			sub->op = awst::BigUIntBinaryOperator::Sub;
			sub->right = std::move(castBiguint);

			// mod 2^64 to wrap
			auto pow2_64_2 = awst::makeIntegerConstant("18446744073709551616", _loc, awst::WType::biguintType());

			auto mod = std::make_shared<awst::BigUIntBinaryOperation>();
			mod->sourceLocation = _loc;
			mod->wtype = awst::WType::biguintType();
			mod->left = std::move(sub);
			mod->op = awst::BigUIntBinaryOperator::Mod;
			mod->right = std::move(pow2_64_2);

			// Back to uint64: safe extract
			auto result = TypeCoercion::implicitNumericCast(std::move(mod), awst::WType::uint64Type(), _loc);
			return wrap(std::move(result));
		}
	}

	case BuilderUnaryOp::BitInvert:
	{
		if (m_isBigUInt)
		{
			// ~x for biguint: use BytesUnaryOperation via ReinterpretCast
			auto toBytes = awst::makeReinterpretCast(resolve(), awst::WType::bytesType(), _loc);

			auto invert = std::make_shared<awst::BytesUnaryOperation>();
			invert->sourceLocation = _loc;
			invert->wtype = awst::WType::bytesType();
			invert->op = awst::BytesUnaryOperator::BitInvert;
			invert->expr = std::move(toBytes);

			auto cast = awst::makeReinterpretCast(std::move(invert), awst::WType::biguintType(), _loc);
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
	auto zero = awst::makeIntegerConstant("0", _loc, m_isBigUInt ? awst::WType::biguintType() : awst::WType::uint64Type());

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
	if (m_ctx.inUncheckedBlock)
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

	auto assign = std::make_shared<awst::AssignmentStatement>();
	assign->sourceLocation = _loc;
	assign->target = tmpVar;
	assign->value = std::move(_result);
	m_ctx.prePendingStatements.push_back(std::move(assign));

	auto maxConst = std::make_shared<awst::IntegerConstant>();
	maxConst->sourceLocation = _loc;
	maxConst->wtype = resType;
	if (m_isBigUInt)
	{
		solidity::u256 maxVal = (solidity::u256(1) << m_bits) - 1;
		maxConst->value = maxVal.str();
	}
	else
	{
		uint64_t maxVal = (uint64_t(1) << m_bits) - 1;
		maxConst->value = std::to_string(maxVal);
	}

	auto cmp = awst::makeNumericCompare(tmpVar, awst::NumericComparison::Lte, std::move(maxConst), _loc);

	auto assertStmt = awst::makeExpressionStatement(awst::makeAssert(std::move(cmp), _loc, "overflow"), _loc);
	m_ctx.prePendingStatements.push_back(std::move(assertStmt));

	return tmpVar;
}

// ─────────────────────────────────────────────────────────────────────
// BigUInt shift via setbit(bzero(32), 255-n, 1)
// ─────────────────────────────────────────────────────────────────────

std::shared_ptr<awst::Expression> SolIntegerBuilder::buildBigUIntShift(
	std::shared_ptr<awst::Expression> _value,
	std::shared_ptr<awst::Expression> _shiftAmt,
	bool _isLeftShift,
	awst::SourceLocation const& _loc)
{
	// bzero(32)
	auto thirtyTwo = awst::makeIntegerConstant("32", _loc);

	auto bzero = awst::makeIntrinsicCall("bzero", awst::WType::bytesType(), _loc);
	bzero->stackArgs.push_back(std::move(thirtyTwo));

	// 255 - n
	auto twoFiftyFive = awst::makeIntegerConstant("255", _loc);

	auto bitIdx = awst::makeUInt64BinOp(std::move(twoFiftyFive), awst::UInt64BinaryOperator::Sub, std::move(_shiftAmt), _loc);

	// setbit(bzero(32), 255-n, 1)
	auto one = awst::makeIntegerConstant("1", _loc);

	auto setbit = awst::makeIntrinsicCall("setbit", awst::WType::bytesType(), _loc);
	setbit->stackArgs.push_back(std::move(bzero));
	setbit->stackArgs.push_back(std::move(bitIdx));
	setbit->stackArgs.push_back(std::move(one));

	auto castPow = awst::makeReinterpretCast(std::move(setbit), awst::WType::biguintType(), _loc);

	auto e = std::make_shared<awst::BigUIntBinaryOperation>();
	e->sourceLocation = _loc;
	e->wtype = awst::WType::biguintType();
	e->left = std::move(_value);
	e->right = std::move(castPow);
	e->op = _isLeftShift ? awst::BigUIntBinaryOperator::Mult : awst::BigUIntBinaryOperator::FloorDiv;

	std::shared_ptr<awst::Expression> result = e;

	// Left shift must wrap mod 2^256 (EVM semantics)
	if (_isLeftShift)
		result = wrapMod256(std::move(result), _loc);

	return result;
}

// ─────────────────────────────────────────────────────────────────────
// BigUInt exponentiation via square-and-multiply
// ─────────────────────────────────────────────────────────────────────

std::shared_ptr<awst::Expression> SolIntegerBuilder::buildBigUIntExp(
	std::shared_ptr<awst::Expression> _base,
	std::shared_ptr<awst::Expression> _exp,
	awst::SourceLocation const& _loc)
{
	_base = TypeCoercion::implicitNumericCast(std::move(_base), awst::WType::biguintType(), _loc);
	_exp = TypeCoercion::implicitNumericCast(std::move(_exp), awst::WType::biguintType(), _loc);

	static int expCounter = 0;
	int id = expCounter++;
	std::string resultVar = "__biguint_exp_result_" + std::to_string(id);
	std::string baseVar = "__biguint_exp_base_" + std::to_string(id);
	std::string expVar = "__biguint_exp_exp_" + std::to_string(id);

	auto makeVar = [&](std::string const& name) {
		auto v = awst::makeVarExpression(name, awst::WType::biguintType(), _loc);
		return v;
	};
	auto makeConst = [&](std::string const& value) {
		auto c = awst::makeIntegerConstant(value, _loc, awst::WType::biguintType());
		return c;
	};
	auto makeAssign = [&](std::string const& target, std::shared_ptr<awst::Expression> value) {
		auto a = std::make_shared<awst::AssignmentStatement>();
		a->sourceLocation = _loc;
		a->target = makeVar(target);
		a->value = std::move(value);
		return a;
	};
	auto makeBinOp = [&](std::shared_ptr<awst::Expression> l, awst::BigUIntBinaryOperator op,
		std::shared_ptr<awst::Expression> r) {
		auto b = std::make_shared<awst::BigUIntBinaryOperation>();
		b->sourceLocation = _loc;
		b->wtype = awst::WType::biguintType();
		b->left = std::move(l);
		b->op = op;
		b->right = std::move(r);
		return b;
	};

	m_ctx.prePendingStatements.push_back(makeAssign(resultVar, makeConst("1")));
	m_ctx.prePendingStatements.push_back(makeAssign(baseVar, std::move(_base)));
	m_ctx.prePendingStatements.push_back(makeAssign(expVar, std::move(_exp)));

	// while exp > 0:
	auto loop = std::make_shared<awst::WhileLoop>();
	loop->sourceLocation = _loc;
	{
		auto cond = awst::makeNumericCompare(makeVar(expVar), awst::NumericComparison::Gt, makeConst("0"), _loc);
		loop->condition = std::move(cond);
	}

	auto body = std::make_shared<awst::Block>();
	body->sourceLocation = _loc;

	// In unchecked mode, Solidity wraps intermediate products mod 2^256 so
	// that huge exponents (e.g. 2**1113) don't blow past biguint capacity.
	bool const wrapMod = m_ctx.inUncheckedBlock;
		auto wrapMod256 = [&](std::shared_ptr<awst::Expression> v)
		-> std::shared_ptr<awst::Expression>
	{
		if (!wrapMod) return v;
		return makeBinOp(std::move(v), awst::BigUIntBinaryOperator::Mod, makeConst(kPow2_256));
	};

	// if exp & 1 != 0: result = (result * base) [mod 2^256 if unchecked]
	{
		auto expAnd1 = makeBinOp(makeVar(expVar), awst::BigUIntBinaryOperator::BitAnd, makeConst("1"));
		auto isOdd = awst::makeNumericCompare(std::move(expAnd1), awst::NumericComparison::Ne, makeConst("0"), _loc);

		std::shared_ptr<awst::Expression> product =
			makeBinOp(makeVar(resultVar), awst::BigUIntBinaryOperator::Mult, makeVar(baseVar));
		product = wrapMod256(std::move(product));

		auto ifBlock = std::make_shared<awst::Block>();
		ifBlock->sourceLocation = _loc;
		ifBlock->body.push_back(makeAssign(resultVar, std::move(product)));

		auto ifStmt = std::make_shared<awst::IfElse>();
		ifStmt->sourceLocation = _loc;
		ifStmt->condition = std::move(isOdd);
		ifStmt->ifBranch = std::move(ifBlock);
		body->body.push_back(std::move(ifStmt));
	}

	// exp = exp / 2; base = (base * base) [mod 2^256 if unchecked]
	body->body.push_back(makeAssign(expVar,
		makeBinOp(makeVar(expVar), awst::BigUIntBinaryOperator::FloorDiv, makeConst("2"))));
	{
		std::shared_ptr<awst::Expression> baseSq =
			makeBinOp(makeVar(baseVar), awst::BigUIntBinaryOperator::Mult, makeVar(baseVar));
		baseSq = wrapMod256(std::move(baseSq));
		body->body.push_back(makeAssign(baseVar, std::move(baseSq)));
	}

	loop->loopBody = std::move(body);
	m_ctx.prePendingStatements.push_back(std::move(loop));

	return makeVar(resultVar);
}

// ─────────────────────────────────────────────────────────────────────
// Wrapping subtraction: (a + 2^256 - b) % 2^256
// ─────────────────────────────────────────────────────────────────────

std::shared_ptr<awst::Expression> SolIntegerBuilder::buildWrappingSubtract(
	std::shared_ptr<awst::Expression> _left,
	std::shared_ptr<awst::Expression> _right,
	awst::SourceLocation const& _loc)
{
	// Checked subtraction: assert a >= b before wrapping
	if (!m_ctx.inUncheckedBlock)
	{
		auto cmp = awst::makeNumericCompare(_left, awst::NumericComparison::Gte, _right, _loc);

		auto assertStmt = awst::makeExpressionStatement(awst::makeAssert(std::move(cmp), _loc, "underflow"), _loc);
		m_ctx.prePendingStatements.push_back(std::move(assertStmt));
	}

	// (a + 2^256 - b) % 2^256
	auto pow256 = awst::makeIntegerConstant(kPow2_256, _loc, awst::WType::biguintType());

	auto addPow = std::make_shared<awst::BigUIntBinaryOperation>();
	addPow->sourceLocation = _loc;
	addPow->wtype = awst::WType::biguintType();
	addPow->left = std::move(_left);
	addPow->op = awst::BigUIntBinaryOperator::Add;
	addPow->right = pow256;

	auto diff = std::make_shared<awst::BigUIntBinaryOperation>();
	diff->sourceLocation = _loc;
	diff->wtype = awst::WType::biguintType();
	diff->left = std::move(addPow);
	diff->op = awst::BigUIntBinaryOperator::Sub;
	diff->right = std::move(_right);

	return wrapMod256(std::move(diff), _loc);
}

// ─────────────────────────────────────────────────────────────────────
// Wrap mod 2^256
// ─────────────────────────────────────────────────────────────────────

std::shared_ptr<awst::Expression> SolIntegerBuilder::wrapMod256(
	std::shared_ptr<awst::Expression> _expr,
	awst::SourceLocation const& _loc)
{
	auto pow256 = awst::makeIntegerConstant(kPow2_256, _loc, awst::WType::biguintType());

	auto mod = std::make_shared<awst::BigUIntBinaryOperation>();
	mod->sourceLocation = _loc;
	mod->wtype = awst::WType::biguintType();
	mod->left = std::move(_expr);
	mod->op = awst::BigUIntBinaryOperator::Mod;
	mod->right = std::move(pow256);
	return mod;
}

// ─────────────────────────────────────────────────────────────────────
// Signed mod/div: operate on absolute values, then apply sign
// ─────────────────────────────────────────────────────────────────────

std::shared_ptr<awst::Expression> SolIntegerBuilder::buildSignedModDiv(
	std::shared_ptr<awst::Expression> _left,
	std::shared_ptr<awst::Expression> _right,
	BuilderBinaryOp _op,
	awst::SourceLocation const& _loc)
{
	// Two's complement: negative if value >= 2^255
	static constexpr char const* POW_2_255 =
		"57896044618658097711785492504343953926634992332820282019728792003956564819968";

	auto makeConst = [&](char const* val) {
		auto c = awst::makeIntegerConstant(val, _loc, awst::WType::biguintType());
		return c;
	};

	// isLeftNeg = left >= 2^255
	auto isLeftNeg = awst::makeNumericCompare(_left, awst::NumericComparison::Gte, makeConst(POW_2_255), _loc);

	// absLeft = isLeftNeg ? (2^256 - left) : left
	auto negLeft = std::make_shared<awst::BigUIntBinaryOperation>();
	negLeft->sourceLocation = _loc;
	negLeft->wtype = awst::WType::biguintType();
	negLeft->left = makeConst(kPow2_256);
	negLeft->op = awst::BigUIntBinaryOperator::Sub;
	negLeft->right = _left;

	auto absLeft = std::make_shared<awst::ConditionalExpression>();
	absLeft->sourceLocation = _loc;
	absLeft->wtype = awst::WType::biguintType();
	absLeft->condition = isLeftNeg;
	absLeft->trueExpr = std::move(negLeft);
	absLeft->falseExpr = _left;

	// isRightNeg = right >= 2^255
	auto isRightNeg = awst::makeNumericCompare(_right, awst::NumericComparison::Gte, makeConst(POW_2_255), _loc);

	// absRight = isRightNeg ? (2^256 - right) : right
	auto negRight = std::make_shared<awst::BigUIntBinaryOperation>();
	negRight->sourceLocation = _loc;
	negRight->wtype = awst::WType::biguintType();
	negRight->left = makeConst(kPow2_256);
	negRight->op = awst::BigUIntBinaryOperator::Sub;
	negRight->right = _right;

	auto absRight = std::make_shared<awst::ConditionalExpression>();
	absRight->sourceLocation = _loc;
	absRight->wtype = awst::WType::biguintType();
	absRight->condition = isRightNeg;
	absRight->trueExpr = std::move(negRight);
	absRight->falseExpr = _right;

	// Compute abs result
	awst::BigUIntBinaryOperator unsignedOp =
		(_op == BuilderBinaryOp::Mod)
		? awst::BigUIntBinaryOperator::Mod
		: awst::BigUIntBinaryOperator::FloorDiv;

	auto absResult = std::make_shared<awst::BigUIntBinaryOperation>();
	absResult->sourceLocation = _loc;
	absResult->wtype = awst::WType::biguintType();
	absResult->left = std::move(absLeft);
	absResult->op = unsignedOp;
	absResult->right = std::move(absRight);

	// Apply sign:
	// mod: sign follows dividend (left)
	// div: sign is negative if signs differ
	auto negResult = std::make_shared<awst::BigUIntBinaryOperation>();
	negResult->sourceLocation = _loc;
	negResult->wtype = awst::WType::biguintType();
	negResult->left = makeConst(kPow2_256);
	negResult->op = awst::BigUIntBinaryOperator::Sub;
	negResult->right = absResult;

	std::shared_ptr<awst::Expression> shouldNegate;
	if (_op == BuilderBinaryOp::Mod)
	{
		shouldNegate = isLeftNeg;
	}
	else
	{
		// div: negate if signs differ — XOR = eitherNeg && !bothNeg
		auto bothNeg = std::make_shared<awst::BooleanBinaryOperation>();
		bothNeg->sourceLocation = _loc;
		bothNeg->wtype = awst::WType::boolType();
		bothNeg->left = isLeftNeg;
		bothNeg->op = awst::BinaryBooleanOperator::And;
		bothNeg->right = isRightNeg;

		auto eitherNeg = std::make_shared<awst::BooleanBinaryOperation>();
		eitherNeg->sourceLocation = _loc;
		eitherNeg->wtype = awst::WType::boolType();
		eitherNeg->left = isLeftNeg;
		eitherNeg->op = awst::BinaryBooleanOperator::Or;
		eitherNeg->right = isRightNeg;

		auto notBothNeg = std::make_shared<awst::Not>();
		notBothNeg->sourceLocation = _loc;
		notBothNeg->wtype = awst::WType::boolType();
		notBothNeg->expr = std::move(bothNeg);

		auto xorSigns = std::make_shared<awst::BooleanBinaryOperation>();
		xorSigns->sourceLocation = _loc;
		xorSigns->wtype = awst::WType::boolType();
		xorSigns->left = std::move(eitherNeg);
		xorSigns->op = awst::BinaryBooleanOperator::And;
		xorSigns->right = std::move(notBothNeg);
		shouldNegate = std::move(xorSigns);
	}

	// Only negate if result is non-zero (negating 0 gives 2^256)
	auto isZero = awst::makeNumericCompare(absResult, awst::NumericComparison::Eq, makeConst("0"), _loc);

	auto notZero = std::make_shared<awst::Not>();
	notZero->sourceLocation = _loc;
	notZero->wtype = awst::WType::boolType();
	notZero->expr = std::move(isZero);

	auto shouldNegateAndNonZero = std::make_shared<awst::BooleanBinaryOperation>();
	shouldNegateAndNonZero->sourceLocation = _loc;
	shouldNegateAndNonZero->wtype = awst::WType::boolType();
	shouldNegateAndNonZero->left = std::move(shouldNegate);
	shouldNegateAndNonZero->op = awst::BinaryBooleanOperator::And;
	shouldNegateAndNonZero->right = std::move(notZero);

	auto signedResult = std::make_shared<awst::ConditionalExpression>();
	signedResult->sourceLocation = _loc;
	signedResult->wtype = awst::WType::biguintType();
	signedResult->condition = std::move(shouldNegateAndNonZero);
	signedResult->trueExpr = std::move(negResult);
	signedResult->falseExpr = std::move(absResult);

	return signedResult;
}

} // namespace puyasol::builder::eb
