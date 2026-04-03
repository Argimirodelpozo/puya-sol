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
		auto bigConst = std::make_shared<awst::IntegerConstant>();
		bigConst->sourceLocation = _loc;
		bigConst->wtype = awst::WType::biguintType();
		bigConst->value = intConst->value;
		return bigConst;
	}

	// itob → ReinterpretCast to biguint
	auto itob = std::make_shared<awst::IntrinsicCall>();
	itob->sourceLocation = _loc;
	itob->wtype = awst::WType::bytesType();
	itob->opCode = "itob";
	itob->stackArgs.push_back(std::move(_expr));

	auto cast = std::make_shared<awst::ReinterpretCast>();
	cast->sourceLocation = _loc;
	cast->wtype = awst::WType::biguintType();
	cast->expr = std::move(itob);
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
	case BuilderBinaryOp::Sub: e->op = awst::UInt64BinaryOperator::Sub; break;
	case BuilderBinaryOp::Mult: e->op = awst::UInt64BinaryOperator::Mult; break;
	case BuilderBinaryOp::Div:
	case BuilderBinaryOp::FloorDiv: e->op = awst::UInt64BinaryOperator::FloorDiv; break;
	case BuilderBinaryOp::Mod: e->op = awst::UInt64BinaryOperator::Mod; break;
	case BuilderBinaryOp::Pow:
	{
		// AVM `exp` asserts on 0^0. Solidity defines 0**0 = 1.
		e->op = awst::UInt64BinaryOperator::Pow;

		auto zero = std::make_shared<awst::IntegerConstant>();
		zero->sourceLocation = _loc;
		zero->wtype = awst::WType::uint64Type();
		zero->value = "0";

		auto cond = std::make_shared<awst::NumericComparisonExpression>();
		cond->sourceLocation = _loc;
		cond->wtype = awst::WType::boolType();
		cond->lhs = e->right; // y
		cond->op = awst::NumericComparison::Eq;
		cond->rhs = std::move(zero);

		auto one = std::make_shared<awst::IntegerConstant>();
		one->sourceLocation = _loc;
		one->wtype = awst::WType::uint64Type();
		one->value = "1";

		auto ternary = std::make_shared<awst::ConditionalExpression>();
		ternary->sourceLocation = _loc;
		ternary->wtype = awst::WType::uint64Type();
		ternary->condition = std::move(cond);
		ternary->trueExpr = std::move(one);
		ternary->falseExpr = e;
		return wrap(emitOverflowCheck(std::move(ternary), _op, _loc));
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
			auto modConst = std::make_shared<awst::IntegerConstant>();
			modConst->sourceLocation = _loc;
			modConst->wtype = awst::WType::uint64Type();
			modConst->value = std::to_string(modVal);

			auto masked = std::make_shared<awst::UInt64BinaryOperation>();
			masked->sourceLocation = _loc;
			masked->wtype = awst::WType::uint64Type();
			masked->left = std::move(result);
			masked->op = awst::UInt64BinaryOperator::Mod;
			masked->right = std::move(modConst);
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
			auto signBit = std::make_shared<awst::IntegerConstant>();
			signBit->sourceLocation = _loc;
			signBit->wtype = awst::WType::biguintType();
			signBit->value = signBitVal.str();

			auto xorL = std::make_shared<awst::BigUIntBinaryOperation>();
			xorL->sourceLocation = _loc;
			xorL->wtype = awst::WType::biguintType();
			xorL->left = std::move(lhs);
			xorL->op = awst::BigUIntBinaryOperator::BitXor;
			xorL->right = signBit;
			lhs = std::move(xorL);

			auto signBit2 = std::make_shared<awst::IntegerConstant>();
			signBit2->sourceLocation = _loc;
			signBit2->wtype = awst::WType::biguintType();
			signBit2->value = signBitVal.str();

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

			auto xorL = std::make_shared<awst::UInt64BinaryOperation>();
			xorL->sourceLocation = _loc;
			xorL->wtype = awst::WType::uint64Type();
			xorL->left = std::move(lhs);
			xorL->op = awst::UInt64BinaryOperator::BitXor;
			xorL->right = signBit;
			lhs = std::move(xorL);

			auto signBit2 = std::make_shared<awst::IntegerConstant>();
			signBit2->sourceLocation = _loc;
			signBit2->wtype = awst::WType::uint64Type();
			signBit2->value = "9223372036854775808";

			auto xorR = std::make_shared<awst::UInt64BinaryOperation>();
			xorR->sourceLocation = _loc;
			xorR->wtype = awst::WType::uint64Type();
			xorR->left = std::move(rhs);
			xorR->op = awst::UInt64BinaryOperator::BitXor;
			xorR->right = std::move(signBit2);
			rhs = std::move(xorR);
		}
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
						auto e = std::make_shared<awst::IntegerConstant>();
						e->sourceLocation = _loc;
						e->wtype = awst::WType::biguintType();
						e->value = result.str();
						return wrap(std::move(e));
					}
					unsigned long long result = (UINT64_MAX - val) + 1ULL;
					auto e = std::make_shared<awst::IntegerConstant>();
					e->sourceLocation = _loc;
					e->wtype = awst::WType::uint64Type();
					e->value = std::to_string(result);
					return wrap(std::move(e));
				}
			}
			catch (...) {} // fall through
		}
		// Runtime negation
		if (m_isBigUInt)
		{
			// Two's complement: -x = ~x + 1
			// Guard: ~0 + 1 = 2^256 overflows 256-bit biguint, so mask with % 2^256
			auto castToBytes = std::make_shared<awst::ReinterpretCast>();
			castToBytes->sourceLocation = _loc;
			castToBytes->wtype = awst::WType::bytesType();
			castToBytes->expr = std::move(operand);

			auto bitInvert = std::make_shared<awst::BytesUnaryOperation>();
			bitInvert->sourceLocation = _loc;
			bitInvert->wtype = awst::WType::bytesType();
			bitInvert->op = awst::BytesUnaryOperator::BitInvert;
			bitInvert->expr = std::move(castToBytes);

			auto castBack = std::make_shared<awst::ReinterpretCast>();
			castBack->sourceLocation = _loc;
			castBack->wtype = awst::WType::biguintType();
			castBack->expr = std::move(bitInvert);

			auto one = std::make_shared<awst::IntegerConstant>();
			one->sourceLocation = _loc;
			one->wtype = awst::WType::biguintType();
			one->value = "1";

			auto addOne = std::make_shared<awst::BigUIntBinaryOperation>();
			addOne->sourceLocation = _loc;
			addOne->wtype = awst::WType::biguintType();
			addOne->left = std::move(castBack);
			addOne->op = awst::BigUIntBinaryOperator::Add;
			addOne->right = std::move(one);

			// Mod 2^256 to handle -0 overflow (2^256 wraps to 0)
			static const std::string pow256 =
				"115792089237316195423570985008687907853269984665640564039457584007913129639936";
			auto modConst = std::make_shared<awst::IntegerConstant>();
			modConst->sourceLocation = _loc;
			modConst->wtype = awst::WType::biguintType();
			modConst->value = pow256;

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
			auto itob = std::make_shared<awst::IntrinsicCall>();
			itob->sourceLocation = _loc;
			itob->wtype = awst::WType::bytesType();
			itob->opCode = "itob";
			itob->stackArgs.push_back(std::move(operand));

			auto castBiguint = std::make_shared<awst::ReinterpretCast>();
			castBiguint->sourceLocation = _loc;
			castBiguint->wtype = awst::WType::biguintType();
			castBiguint->expr = std::move(itob);

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
			auto pow2_64_2 = std::make_shared<awst::IntegerConstant>();
			pow2_64_2->sourceLocation = _loc;
			pow2_64_2->wtype = awst::WType::biguintType();
			pow2_64_2->value = "18446744073709551616";

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
			auto toBytes = std::make_shared<awst::ReinterpretCast>();
			toBytes->sourceLocation = _loc;
			toBytes->wtype = awst::WType::bytesType();
			toBytes->expr = resolve();

			auto invert = std::make_shared<awst::BytesUnaryOperation>();
			invert->sourceLocation = _loc;
			invert->wtype = awst::WType::bytesType();
			invert->op = awst::BytesUnaryOperator::BitInvert;
			invert->expr = std::move(toBytes);

			auto cast = std::make_shared<awst::ReinterpretCast>();
			cast->sourceLocation = _loc;
			cast->wtype = awst::WType::biguintType();
			cast->expr = std::move(invert);
			return wrap(std::move(cast));
		}
		// ~x for uint64: XOR with bit-width mask (not always 2^64-1)
		{
			solidity::u256 mask = (m_bits >= 64)
				? solidity::u256("18446744073709551615")
				: (solidity::u256(1) << m_bits) - 1;
			std::ostringstream oss;
			oss << mask;

			auto maxVal = std::make_shared<awst::IntegerConstant>();
			maxVal->sourceLocation = _loc;
			maxVal->wtype = awst::WType::uint64Type();
			maxVal->value = oss.str();

			auto e = std::make_shared<awst::UInt64BinaryOperation>();
			e->sourceLocation = _loc;
			e->wtype = awst::WType::uint64Type();
			e->left = resolve();
			e->op = awst::UInt64BinaryOperator::BitXor;
			e->right = std::move(maxVal);
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
	auto zero = std::make_shared<awst::IntegerConstant>();
	zero->sourceLocation = _loc;
	zero->wtype = m_isBigUInt ? awst::WType::biguintType() : awst::WType::uint64Type();
	zero->value = "0";

	auto cmp = std::make_shared<awst::NumericComparisonExpression>();
	cmp->sourceLocation = _loc;
	cmp->wtype = awst::WType::boolType();
	cmp->lhs = resolve();
	cmp->rhs = std::move(zero);
	cmp->op = _negate ? awst::NumericComparison::Eq : awst::NumericComparison::Ne;

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

	auto tmpVar = std::make_shared<awst::VarExpression>();
	tmpVar->sourceLocation = _loc;
	tmpVar->wtype = resType;
	tmpVar->name = tmpName;

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

	auto cmp = std::make_shared<awst::NumericComparisonExpression>();
	cmp->sourceLocation = _loc;
	cmp->wtype = awst::WType::boolType();
	cmp->lhs = tmpVar;
	cmp->op = awst::NumericComparison::Lte;
	cmp->rhs = std::move(maxConst);

	auto assertExpr = std::make_shared<awst::AssertExpression>();
	assertExpr->sourceLocation = _loc;
	assertExpr->wtype = awst::WType::voidType();
	assertExpr->condition = std::move(cmp);
	assertExpr->errorMessage = "overflow";

	auto assertStmt = std::make_shared<awst::ExpressionStatement>();
	assertStmt->sourceLocation = _loc;
	assertStmt->expr = std::move(assertExpr);
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
	auto thirtyTwo = std::make_shared<awst::IntegerConstant>();
	thirtyTwo->sourceLocation = _loc;
	thirtyTwo->wtype = awst::WType::uint64Type();
	thirtyTwo->value = "32";

	auto bzero = std::make_shared<awst::IntrinsicCall>();
	bzero->sourceLocation = _loc;
	bzero->wtype = awst::WType::bytesType();
	bzero->opCode = "bzero";
	bzero->stackArgs.push_back(std::move(thirtyTwo));

	// 255 - n
	auto twoFiftyFive = std::make_shared<awst::IntegerConstant>();
	twoFiftyFive->sourceLocation = _loc;
	twoFiftyFive->wtype = awst::WType::uint64Type();
	twoFiftyFive->value = "255";

	auto bitIdx = std::make_shared<awst::UInt64BinaryOperation>();
	bitIdx->sourceLocation = _loc;
	bitIdx->wtype = awst::WType::uint64Type();
	bitIdx->left = std::move(twoFiftyFive);
	bitIdx->right = std::move(_shiftAmt);
	bitIdx->op = awst::UInt64BinaryOperator::Sub;

	// setbit(bzero(32), 255-n, 1)
	auto one = std::make_shared<awst::IntegerConstant>();
	one->sourceLocation = _loc;
	one->wtype = awst::WType::uint64Type();
	one->value = "1";

	auto setbit = std::make_shared<awst::IntrinsicCall>();
	setbit->sourceLocation = _loc;
	setbit->wtype = awst::WType::bytesType();
	setbit->opCode = "setbit";
	setbit->stackArgs.push_back(std::move(bzero));
	setbit->stackArgs.push_back(std::move(bitIdx));
	setbit->stackArgs.push_back(std::move(one));

	auto castPow = std::make_shared<awst::ReinterpretCast>();
	castPow->sourceLocation = _loc;
	castPow->wtype = awst::WType::biguintType();
	castPow->expr = std::move(setbit);

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
		auto v = std::make_shared<awst::VarExpression>();
		v->sourceLocation = _loc;
		v->name = name;
		v->wtype = awst::WType::biguintType();
		return v;
	};
	auto makeConst = [&](std::string const& value) {
		auto c = std::make_shared<awst::IntegerConstant>();
		c->sourceLocation = _loc;
		c->wtype = awst::WType::biguintType();
		c->value = value;
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
		auto cond = std::make_shared<awst::NumericComparisonExpression>();
		cond->sourceLocation = _loc;
		cond->wtype = awst::WType::boolType();
		cond->lhs = makeVar(expVar);
		cond->op = awst::NumericComparison::Gt;
		cond->rhs = makeConst("0");
		loop->condition = std::move(cond);
	}

	auto body = std::make_shared<awst::Block>();
	body->sourceLocation = _loc;

	// if exp & 1 != 0: result = result * base
	{
		auto expAnd1 = makeBinOp(makeVar(expVar), awst::BigUIntBinaryOperator::BitAnd, makeConst("1"));
		auto isOdd = std::make_shared<awst::NumericComparisonExpression>();
		isOdd->sourceLocation = _loc;
		isOdd->wtype = awst::WType::boolType();
		isOdd->lhs = std::move(expAnd1);
		isOdd->op = awst::NumericComparison::Ne;
		isOdd->rhs = makeConst("0");

		auto ifBlock = std::make_shared<awst::Block>();
		ifBlock->sourceLocation = _loc;
		ifBlock->body.push_back(makeAssign(resultVar,
			makeBinOp(makeVar(resultVar), awst::BigUIntBinaryOperator::Mult, makeVar(baseVar))));

		auto ifStmt = std::make_shared<awst::IfElse>();
		ifStmt->sourceLocation = _loc;
		ifStmt->condition = std::move(isOdd);
		ifStmt->ifBranch = std::move(ifBlock);
		body->body.push_back(std::move(ifStmt));
	}

	// exp = exp / 2; base = base * base
	body->body.push_back(makeAssign(expVar,
		makeBinOp(makeVar(expVar), awst::BigUIntBinaryOperator::FloorDiv, makeConst("2"))));
	body->body.push_back(makeAssign(baseVar,
		makeBinOp(makeVar(baseVar), awst::BigUIntBinaryOperator::Mult, makeVar(baseVar))));

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
		auto cmp = std::make_shared<awst::NumericComparisonExpression>();
		cmp->sourceLocation = _loc;
		cmp->wtype = awst::WType::boolType();
		cmp->lhs = _left;  // shared ref
		cmp->op = awst::NumericComparison::Gte;
		cmp->rhs = _right; // shared ref

		auto assertExpr = std::make_shared<awst::AssertExpression>();
		assertExpr->sourceLocation = _loc;
		assertExpr->wtype = awst::WType::voidType();
		assertExpr->condition = std::move(cmp);
		assertExpr->errorMessage = "underflow";

		auto assertStmt = std::make_shared<awst::ExpressionStatement>();
		assertStmt->sourceLocation = _loc;
		assertStmt->expr = std::move(assertExpr);
		m_ctx.prePendingStatements.push_back(std::move(assertStmt));
	}

	// (a + 2^256 - b) % 2^256
	auto pow256 = std::make_shared<awst::IntegerConstant>();
	pow256->sourceLocation = _loc;
	pow256->wtype = awst::WType::biguintType();
	pow256->value = POW_2_256;

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
	auto pow256 = std::make_shared<awst::IntegerConstant>();
	pow256->sourceLocation = _loc;
	pow256->wtype = awst::WType::biguintType();
	pow256->value = POW_2_256;

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
		auto c = std::make_shared<awst::IntegerConstant>();
		c->sourceLocation = _loc;
		c->wtype = awst::WType::biguintType();
		c->value = val;
		return c;
	};

	// isLeftNeg = left >= 2^255
	auto isLeftNeg = std::make_shared<awst::NumericComparisonExpression>();
	isLeftNeg->sourceLocation = _loc;
	isLeftNeg->wtype = awst::WType::boolType();
	isLeftNeg->lhs = _left;
	isLeftNeg->op = awst::NumericComparison::Gte;
	isLeftNeg->rhs = makeConst(POW_2_255);

	// absLeft = isLeftNeg ? (2^256 - left) : left
	auto negLeft = std::make_shared<awst::BigUIntBinaryOperation>();
	negLeft->sourceLocation = _loc;
	negLeft->wtype = awst::WType::biguintType();
	negLeft->left = makeConst(POW_2_256);
	negLeft->op = awst::BigUIntBinaryOperator::Sub;
	negLeft->right = _left;

	auto absLeft = std::make_shared<awst::ConditionalExpression>();
	absLeft->sourceLocation = _loc;
	absLeft->wtype = awst::WType::biguintType();
	absLeft->condition = isLeftNeg;
	absLeft->trueExpr = std::move(negLeft);
	absLeft->falseExpr = _left;

	// isRightNeg = right >= 2^255
	auto isRightNeg = std::make_shared<awst::NumericComparisonExpression>();
	isRightNeg->sourceLocation = _loc;
	isRightNeg->wtype = awst::WType::boolType();
	isRightNeg->lhs = _right;
	isRightNeg->op = awst::NumericComparison::Gte;
	isRightNeg->rhs = makeConst(POW_2_255);

	// absRight = isRightNeg ? (2^256 - right) : right
	auto negRight = std::make_shared<awst::BigUIntBinaryOperation>();
	negRight->sourceLocation = _loc;
	negRight->wtype = awst::WType::biguintType();
	negRight->left = makeConst(POW_2_256);
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
	negResult->left = makeConst(POW_2_256);
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
	auto isZero = std::make_shared<awst::NumericComparisonExpression>();
	isZero->sourceLocation = _loc;
	isZero->wtype = awst::WType::boolType();
	isZero->lhs = absResult;
	isZero->op = awst::NumericComparison::Eq;
	isZero->rhs = makeConst("0");

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
