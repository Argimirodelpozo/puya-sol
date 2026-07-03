#include "builder/sol-eb/BinaryOpBuilder.h"

#include "builder/sol-eb/BigUIntMathHelpers.h"
#include "builder/sol-eb/ContractContext.h"
#include "builder/sol-types/TypeCoercion.h"
#include "Logger.h"

namespace puyasol::builder::eb
{

namespace
{
bool isBigUInt(awst::WType const* _type)
{
	return _type == awst::WType::biguintType();
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
	using Token = solidity::frontend::Token;

	// Coerce bytes[N] to numeric. >8-byte or unknown-length (e.g. keccak256 32-byte
	// digest typed `bytes`) → biguint via ReinterpretCast; btoi only handles ≤8 bytes.
	auto coerceBytesToUint = [&](std::shared_ptr<awst::Expression>& operand) {
		if (operand->wtype && operand->wtype->kind() == awst::WTypeKind::Bytes)
		{
			auto const* bytesWType = dynamic_cast<awst::BytesWType const*>(operand->wtype);
			bool knownSmall =
				bytesWType && bytesWType->length().has_value() && *bytesWType->length() <= 8;
			if (!knownSmall)
			{
				auto cast = awst::makeAsBiguint(std::move(operand), _loc);
				operand = std::move(cast);
				return;
			}
			auto expr = std::move(operand);
			if (expr->wtype != awst::WType::bytesType())
			{
				auto toBytes = awst::makeAsBytes(std::move(expr), _loc);
				expr = std::move(toBytes);
			}
			operand = awst::makeBtoi(std::move(expr), _loc);
		}
	};

	bool leftIsBytes = _left->wtype && _left->wtype->kind() == awst::WTypeKind::Bytes;
	bool rightIsBytes = _right->wtype && _right->wtype->kind() == awst::WTypeKind::Bytes;
	bool leftIsNumeric = _left->wtype == awst::WType::uint64Type()
		|| _left->wtype == awst::WType::biguintType();
	bool rightIsNumeric = _right->wtype == awst::WType::uint64Type()
		|| _right->wtype == awst::WType::biguintType();
	if (leftIsBytes && rightIsNumeric)
		coerceBytesToUint(_left);
	if (rightIsBytes && leftIsNumeric)
		coerceBytesToUint(_right);

	auto promoteToBigUInt = [&](std::shared_ptr<awst::Expression>& operand) {
		// uint64-only, DELIBERATELY narrower than eb::promoteToBiguint's catch-all:
		// bytes operands here either already went through coerceBytesToUint above or
		// are legitimately compared as raw bytes — reinterpreting them to biguint
		// would change those comparisons (and >64-byte values can't be biguint).
		if (operand->wtype == awst::WType::uint64Type())
			operand = promoteToBiguint(std::move(operand), _loc);
	};

	// Comparison operations
	switch (_op)
	{
	case Token::Equal:
	case Token::NotEqual:
	case Token::LessThan:
	case Token::LessThanOrEqual:
	case Token::GreaterThan:
	case Token::GreaterThanOrEqual:
	{
		bool isBytesBacked = _left->wtype == awst::WType::accountType()
			|| (_left->wtype && _left->wtype->kind() == awst::WTypeKind::Bytes)
			|| _left->wtype == awst::WType::stringType();

		if (isBytesBacked && (_op == Token::Equal || _op == Token::NotEqual))
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
				(_op == Token::Equal) ? awst::EqualityComparison::Eq : awst::EqualityComparison::Ne,
				std::move(_right), _loc);
		}

		// Bytes ordering: AVM b</b>/b<=/b>= intrinsics.
		if (isBytesBacked)
		{
			std::string opCode;
			switch (_op)
			{
			case Token::LessThan: opCode = "b<"; break;
			case Token::LessThanOrEqual: opCode = "b<="; break;
			case Token::GreaterThan: opCode = "b>"; break;
			case Token::GreaterThanOrEqual: opCode = "b>="; break;
			default: break;
			}
			if (!opCode.empty())
			{
				auto e = awst::makeIntrinsicCall(std::move(opCode), awst::WType::boolType(), _loc);
				e->stackArgs.push_back(std::move(_left));
				e->stackArgs.push_back(std::move(_right));
				return e;
			}
		}

		if (isBigUInt(_left->wtype) != isBigUInt(_right->wtype))
		{
			promoteToBigUInt(_left);
			promoteToBigUInt(_right);
		}

		awst::NumericComparison cmpOp = awst::NumericComparison::Eq;
		switch (_op)
		{
		case Token::Equal: cmpOp = awst::NumericComparison::Eq; break;
		case Token::NotEqual: cmpOp = awst::NumericComparison::Ne; break;
		case Token::LessThan: cmpOp = awst::NumericComparison::Lt; break;
		case Token::LessThanOrEqual: cmpOp = awst::NumericComparison::Lte; break;
		case Token::GreaterThan: cmpOp = awst::NumericComparison::Gt; break;
		case Token::GreaterThanOrEqual: cmpOp = awst::NumericComparison::Gte; break;
		default: break;
		}
		return awst::makeNumericCompare(std::move(_left), cmpOp, std::move(_right), _loc);
	}

	// NB no And/Or here: SolBinaryOperation::trySolShortCircuit handles every
	// `&&`/`||` (it is total for those tokens — single nullptr return is the
	// not-And/Or early-out), so they can never reach this fallback.

	default:
		break;
	}

	// Literal-base `**` (e.g. `2 ** x`): the rational-typed base has no eb
	// instance builder, so the exp lands here. Every other biguint arithmetic
	// shape is owned by the eb builders now.
	if (_op == Token::Exp
		&& (isBigUInt(_resultType) || isBigUInt(_left->wtype) || isBigUInt(_right->wtype)))
	{
		promoteToBigUInt(_left);
		promoteToBigUInt(_right);
		// biguint ** : no AVM opcode; emit square-and-multiply loop (shared helper).
		return buildBigUIntExp(
			_ctx, _scope.isUnchecked(), std::move(_left), std::move(_right), _loc);
	}

	// uint64 bitwise: live via the bytes1-element compound path (`b[i] |= x` —
	// the bytes operand was btoi'd to uint64 by the coercion preamble above).
	switch (_op)
	{
	case Token::BitOr: case Token::AssignBitOr:
	case Token::BitXor: case Token::AssignBitXor:
	case Token::BitAnd: case Token::AssignBitAnd:
	{
		auto e = std::make_shared<awst::UInt64BinaryOperation>();
		e->sourceLocation = _loc;
		e->wtype = awst::WType::uint64Type();
		e->left = std::move(_left);
		e->right = std::move(_right);
		e->op = (_op == Token::BitOr || _op == Token::AssignBitOr)
				? awst::UInt64BinaryOperator::BitOr
			: (_op == Token::BitXor || _op == Token::AssignBitXor)
				? awst::UInt64BinaryOperator::BitXor
				: awst::UInt64BinaryOperator::BitAnd;
		return e;
	}
	default:
		break;
	}

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
