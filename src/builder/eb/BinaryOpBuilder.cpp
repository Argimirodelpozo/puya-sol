#include "builder/eb/BinaryOpBuilder.h"

#include "builder/eb/BigUIntMathHelpers.h"
#include "builder/context/ContractContext.h"
#include "builder/eb/SolFixedBytesBuilder.h" // padBytesOperandsToCommonWidth
#include "builder/types/TypeCoercion.h"
#include "builder/eb/BuilderOps.h"
#include "Logger.h"

#include <optional>
#include <string>

namespace puyasol::builder::eb
{

std::shared_ptr<awst::Expression> buildBytesComparison(
	awst::NumericComparison _op,
	std::shared_ptr<awst::Expression> _left,
	std::shared_ptr<awst::Expression> _right,
	awst::SourceLocation const& _loc)
{
	if (_op == awst::NumericComparison::Eq || _op == awst::NumericComparison::Ne)
	{
		if (_left->wtype != _right->wtype)
		{
			auto castToBytes = [&](std::shared_ptr<awst::Expression>& expr) {
				if (expr->wtype != awst::WType::bytesType())
				{
					auto cast = awst::makeAsBytes(std::move(expr), _loc);
					expr = std::move(cast);
				}
			};
			castToBytes(_left);
			castToBytes(_right);
		}
		return awst::makeBytesComparison(std::move(_left),
			(_op == awst::NumericComparison::Eq) ? awst::EqualityComparison::Eq : awst::EqualityComparison::Ne,
			std::move(_right), _loc);
	}

	// Bytes ordering: AVM b</b>/b<=/b>= intrinsics.
	std::string opCode;
	switch (_op)
	{
	case awst::NumericComparison::Lt: opCode = "b<"; break;
	case awst::NumericComparison::Lte: opCode = "b<="; break;
	case awst::NumericComparison::Gt: opCode = "b>"; break;
	case awst::NumericComparison::Gte: opCode = "b>="; break;
	default: throw std::logic_error("Not an ordered comparison");
	}
	auto e = awst::makeIntrinsicCall(std::move(opCode), awst::WType::boolType(), _loc);
	e->stackArgs.push_back(std::move(_left));
	e->stackArgs.push_back(std::move(_right));
	return e;
}

namespace
{
using Token = solidity::frontend::Token;

bool isBigUInt(awst::WType const* _type)
{
	return _type == awst::WType::biguintType();
}

// Coerce bytes[N] to numeric. >8-byte or unknown-length (e.g. keccak256 32-byte
// digest typed `bytes`) → biguint via ReinterpretCast; btoi only handles ≤8 bytes.
void coerceBytesToUint(
	std::shared_ptr<awst::Expression>& _operand, awst::SourceLocation const& _loc)
{
	if (_operand->wtype && _operand->wtype->kind() == awst::WTypeKind::Bytes)
	{
		auto const* bytesWType = dynamic_cast<awst::BytesWType const*>(_operand->wtype);
		bool knownSmall =
			bytesWType && bytesWType->length().has_value() && *bytesWType->length() <= 8;
		if (!knownSmall)
		{
			auto cast = awst::makeAsBiguint(std::move(_operand), _loc);
			_operand = std::move(cast);
			return;
		}
		auto expr = std::move(_operand);
		if (expr->wtype != awst::WType::bytesType())
		{
			auto toBytes = awst::makeAsBytes(std::move(expr), _loc);
			expr = std::move(toBytes);
		}
		_operand = awst::makeBtoi(std::move(expr), _loc);
	}
}

// Operand preamble: a bytes operand paired with a NUMERIC (uint64/biguint) one
// is coerced to the numeric domain, either side. Both classifications are
// taken before either coercion runs.
void coerceMixedBytesNumericOperands(
	std::shared_ptr<awst::Expression>& _left,
	std::shared_ptr<awst::Expression>& _right,
	awst::SourceLocation const& _loc)
{
	bool leftIsBytes = _left->wtype && _left->wtype->kind() == awst::WTypeKind::Bytes;
	bool rightIsBytes = _right->wtype && _right->wtype->kind() == awst::WTypeKind::Bytes;
	bool leftIsNumeric = _left->wtype == awst::WType::uint64Type()
		|| _left->wtype == awst::WType::biguintType();
	bool rightIsNumeric = _right->wtype == awst::WType::uint64Type()
		|| _right->wtype == awst::WType::biguintType();
	if (leftIsBytes && rightIsNumeric)
		coerceBytesToUint(_left, _loc);
	if (rightIsBytes && leftIsNumeric)
		coerceBytesToUint(_right, _loc);
}

// uint64-only, DELIBERATELY narrower than eb::promoteToBiguint's catch-all:
// bytes operands here either already went through coerceBytesToUint above or
// are legitimately compared as raw bytes — reinterpreting them to biguint
// would change those comparisons (and >64-byte values can't be biguint).
void promoteUInt64ToBigUInt(
	std::shared_ptr<awst::Expression>& _operand, awst::SourceLocation const& _loc)
{
	if (_operand->wtype == awst::WType::uint64Type())
		_operand = promoteToBiguint(std::move(_operand), _loc);
}

// ── Comparison family ────────────────────────────────────────────────

// The six comparison tokens: bytes-backed left operands compare as bytes,
// everything else numerically (mixed uint64/biguint pairs promoted).
std::shared_ptr<awst::Expression> buildComparison(
	ContractContext& _ctx,
	Token _op,
	std::shared_ptr<awst::Expression> _left,
	std::shared_ptr<awst::Expression> _right,
	awst::SourceLocation const& _loc)
{
	bool isBytesBacked = _left->wtype == awst::WType::accountType()
		|| (_left->wtype && _left->wtype->kind() == awst::WTypeKind::Bytes)
		|| _left->wtype == awst::WType::stringType();

	if (isBytesBacked)
	{
		// EVM left-aligned bytesN compare: pad both sides to the common declared
		// width — equality and ordering both (padBytesOperandsToCommonWidth).
		padBytesOperandsToCommonWidth(_ctx, _left, _right);
		return buildBytesComparison(comparisonOpFor(_op).value(), std::move(_left), std::move(_right), _loc);
	}

	if (isBigUInt(_left->wtype) != isBigUInt(_right->wtype))
	{
		promoteUInt64ToBigUInt(_left, _loc);
		promoteUInt64ToBigUInt(_right, _loc);
	}

	return awst::makeNumericCompare(
		std::move(_left), comparisonOpFor(_op).value(), std::move(_right), _loc);
}

// ── Exp family ───────────────────────────────────────────────────────

// Literal-base `**` (e.g. `2 ** x`): the rational-typed base has no eb
// instance builder, so the exp lands here. Every other biguint arithmetic
// shape is owned by the eb builders now. nullptr = not a biguint `**`.
std::shared_ptr<awst::Expression> tryBigUIntExp(
	ContractContext& _ctx,
	sol_ast::Context& _scope,
	Token _op,
	std::shared_ptr<awst::Expression>& _left,
	std::shared_ptr<awst::Expression>& _right,
	awst::WType const* _resultType,
	awst::SourceLocation const& _loc)
{
	if (!(_op == Token::Exp
		&& (isBigUInt(_resultType) || isBigUInt(_left->wtype) || isBigUInt(_right->wtype))))
		return nullptr;
	promoteUInt64ToBigUInt(_left, _loc);
	promoteUInt64ToBigUInt(_right, _loc);
	// biguint ** : no AVM opcode; emit square-and-multiply loop (shared helper).
	return buildBigUIntExp(
		_ctx, _scope.isUnchecked(), std::move(_left), std::move(_right), _loc);
}

// ── Bitwise family ───────────────────────────────────────────────────

// The uint64 operator for a plain or compound bitwise token; nullopt otherwise.
std::optional<awst::UInt64BinaryOperator> uint64BitwiseOperator(Token _op)
{
	switch (_op)
	{
	case Token::BitOr:
		return awst::UInt64BinaryOperator::BitOr;
	case Token::BitXor:
		return awst::UInt64BinaryOperator::BitXor;
	case Token::BitAnd:
		return awst::UInt64BinaryOperator::BitAnd;
	default:
		return std::nullopt;
	}
}

// uint64 bitwise: live via the bytes1-element compound path (`b[i] |= x` —
// the bytes operand was btoi'd to uint64 by the coercion preamble above).
// nullptr = not a bitwise token, operands untouched.
std::shared_ptr<awst::Expression> tryUInt64Bitwise(
	Token _op,
	std::shared_ptr<awst::Expression>& _left,
	std::shared_ptr<awst::Expression>& _right,
	awst::SourceLocation const& _loc)
{
	auto bitOp = uint64BitwiseOperator(_op);
	if (!bitOp)
		return nullptr;
	auto e = std::make_shared<awst::UInt64BinaryOperation>();
	e->sourceLocation = _loc;
	e->wtype = awst::WType::uint64Type();
	e->left = std::move(_left);
	e->right = std::move(_right);
	e->op = *bitOp;
	return e;
}
} // namespace

std::shared_ptr<awst::Expression> buildBinaryOp(
	ContractContext& _ctx,
	sol_ast::Context& _scope,
	solidity::frontend::Token _op,
	std::shared_ptr<awst::Expression> _left,
	std::shared_ptr<awst::Expression> _right,
	awst::WType const* _resultType,
	awst::SourceLocation const& _loc
)
{
	_op = binaryToken(_op);
	coerceMixedBytesNumericOperands(_left, _right, _loc);

	// Operator families in the order they were checked: comparison, then
	// literal-base biguint `**`, then the uint64 bitwise compound shape.
	if (solidity::langutil::TokenTraits::isCompareOp(_op))
		return buildComparison(_ctx, _op, std::move(_left), std::move(_right), _loc);

	// NB no And/Or here: SolBinaryOperation::trySolShortCircuit handles every
	// `&&`/`||` (it is total for those tokens — single nullptr return is the
	// not-And/Or early-out), so they can never reach this fallback.

	if (auto exp = tryBigUIntExp(_ctx, _scope, _op, _left, _right, _resultType, _loc))
		return exp;

	if (auto bitwise = tryUInt64Bitwise(_op, _left, _right, _loc))
		return bitwise;

	// RETIRED generic fallbacks (fable-review C3, corpus-audited 2026-07-03):
	// arithmetic, shifts, exp-on-uint64, bytes-vs-bytes bitwise and &&/|| used
	// to be lowered here with UNSIGNED/unchecked-blind semantics — every one
	// of those shapes is owned by the eb builders (SolIntegerBuilder,
	// SolFixedBytesBuilder) or trySolShortCircuit. A full-suite + generative-
	// fuzz trace showed only Eq/Ne comparisons, literal-base biguint `**` and
	// the bytes1-element bitwise compound reaching this fallback. Anything
	// else arriving here means a dispatch gap — fail LOUD rather than lower
	// with the wrong semantics (the old silent tail was exactly how signed
	// compound ops once mis-lowered).
	Logger::instance().error(
		"internal: binary operator (token " + std::to_string(static_cast<int>(_op))
			+ ") reached the retired generic fallback — an eb builder should own it",
		_loc);
	return _left;
}

} // namespace puyasol::builder::eb
