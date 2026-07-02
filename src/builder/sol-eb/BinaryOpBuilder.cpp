#include "builder/sol-eb/BinaryOpBuilder.h"

#include "builder/sol-eb/BigUIntMathHelpers.h"
#include "builder/sol-eb/ContractContext.h"
#include "builder/sol-types/TypeCoercion.h"

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

	// Boolean operations
	case Token::And:
	{
		auto e = awst::makeBoolBinOp(std::move(_left), awst::BinaryBooleanOperator::And, std::move(_right), _loc);
		return e;
	}
	case Token::Or:
	{
		auto e = awst::makeBoolBinOp(std::move(_left), awst::BinaryBooleanOperator::Or, std::move(_right), _loc);
		return e;
	}

	default:
		break;
	}

	// Bytes bitwise (b|, b&, b^) for bytes[N] types.
	{
		bool leftIsBytesKind = _left->wtype && _left->wtype->kind() == awst::WTypeKind::Bytes;
		bool rightIsBytesKind = _right->wtype && _right->wtype->kind() == awst::WTypeKind::Bytes;
		bool isBitwiseOp = (_op == Token::BitOr || _op == Token::AssignBitOr
			|| _op == Token::BitXor || _op == Token::AssignBitXor
			|| _op == Token::BitAnd || _op == Token::AssignBitAnd);

		if ((leftIsBytesKind || rightIsBytesKind) && isBitwiseOp)
		{
			awst::BytesBinaryOperator bytesOp = awst::BytesBinaryOperator::BitOr;
			switch (_op)
			{
			case Token::BitOr: case Token::AssignBitOr: bytesOp = awst::BytesBinaryOperator::BitOr; break;
			case Token::BitXor: case Token::AssignBitXor: bytesOp = awst::BytesBinaryOperator::BitXor; break;
			case Token::BitAnd: case Token::AssignBitAnd: bytesOp = awst::BytesBinaryOperator::BitAnd; break;
			default: break;
			}
			return awst::makeBytesBinOp(std::move(_left), bytesOp, std::move(_right), _loc);
		}
	}

	if (isBigUInt(_resultType) || isBigUInt(_left->wtype) || isBigUInt(_right->wtype))
	{
		promoteToBigUInt(_left);

		// biguint has no shift opcode: x<<n = x*(2^n), x>>n = x/(2^n).
		// 2^n via setbit(bzero(32),255-n,1). Shift amount stays uint64.
		if (_op == Token::SHL || _op == Token::AssignShl
			|| _op == Token::SHR || _op == Token::AssignShr
			|| _op == Token::SAR || _op == Token::AssignSar)
		{
			auto shiftAmt = TypeCoercion::implicitNumericCast(std::move(_right), awst::WType::uint64Type(), _loc);

			auto bzero = awst::makeBzero(32, _loc);
			auto twoFiftyFive = awst::makeIntegerConstant("255", _loc);
			auto bitIdx = awst::makeUInt64BinOp(std::move(twoFiftyFive), awst::UInt64BinaryOperator::Sub, std::move(shiftAmt), _loc);
			auto setbit = awst::makeSetbit(
				std::move(bzero), std::move(bitIdx), awst::makeOne(_loc), _loc);
			auto castToBigUInt = awst::makeAsBiguint(std::move(setbit), _loc);

			auto shiftBigOp = (_op == Token::SHL || _op == Token::AssignShl)
				? awst::BigUIntBinaryOperator::Mult
				: awst::BigUIntBinaryOperator::FloorDiv;
			return awst::makeBigUIntBinOp(std::move(_left), shiftBigOp, std::move(castToBigUInt), _loc);
		}

		promoteToBigUInt(_right);

		if (_op == Token::Sub || _op == Token::AssignSub)
			// eval-once both operands so a checked `a - f()` doesn't double-eval f().
			return buildWrappingSubtract(
				_ctx, _scope.isUnchecked(), std::move(_left), std::move(_right), _loc);


		// biguint ** : no AVM opcode; emit square-and-multiply loop (shared helper).
		if (_op == Token::Exp)
		{
			return buildBigUIntExp(
				_ctx, _scope.isUnchecked(), std::move(_left), std::move(_right), _loc);
		}

		awst::BigUIntBinaryOperator bigOp = awst::BigUIntBinaryOperator::Add;
		switch (_op)
		{
		case Token::Add: case Token::AssignAdd: bigOp = awst::BigUIntBinaryOperator::Add; break;
		case Token::Mul: case Token::AssignMul: bigOp = awst::BigUIntBinaryOperator::Mult; break;
		case Token::Div: case Token::AssignDiv: bigOp = awst::BigUIntBinaryOperator::FloorDiv; break;
		case Token::Mod: case Token::AssignMod: bigOp = awst::BigUIntBinaryOperator::Mod; break;
		case Token::BitOr: case Token::AssignBitOr: bigOp = awst::BigUIntBinaryOperator::BitOr; break;
		case Token::BitXor: case Token::AssignBitXor: bigOp = awst::BigUIntBinaryOperator::BitXor; break;
		case Token::BitAnd: case Token::AssignBitAnd: bigOp = awst::BigUIntBinaryOperator::BitAnd; break;
		default: break;
		}
		auto e = awst::makeBigUIntBinOp(std::move(_left), bigOp, std::move(_right), _loc);

		// Unchecked: wrap mod 2^256 (AVM biguint is arbitrary-precision; >256-bit breaks EVM semantics).
		if (_scope.isUnchecked()
			&& (_op == Token::Add || _op == Token::AssignAdd
				|| _op == Token::Sub || _op == Token::AssignSub
				|| _op == Token::Mul || _op == Token::AssignMul))
		{
			auto pow256 = makePow256(_loc);

			auto mod = awst::makeBigUIntBinOp(e, awst::BigUIntBinaryOperator::Mod, std::move(pow256), _loc);
			return mod;
		}

		return e;
	}
	else
	{
		auto e = std::make_shared<awst::UInt64BinaryOperation>();
		e->sourceLocation = _loc;
		e->wtype = awst::WType::uint64Type();
		e->left = std::move(_left);
		e->right = std::move(_right);

		switch (_op)
		{
		case Token::Add: case Token::AssignAdd: e->op = awst::UInt64BinaryOperator::Add; break;
		case Token::Sub: case Token::AssignSub: e->op = awst::UInt64BinaryOperator::Sub; break;
		case Token::Mul: case Token::AssignMul: e->op = awst::UInt64BinaryOperator::Mult; break;
		case Token::Div: case Token::AssignDiv: e->op = awst::UInt64BinaryOperator::FloorDiv; break;
		case Token::Mod: case Token::AssignMod: e->op = awst::UInt64BinaryOperator::Mod; break;
		case Token::Exp:
		{
			// AVM `exp` asserts on 0^0; Solidity defines 0**0=1. Guard: y==0 ? 1 : x**y.
			e->op = awst::UInt64BinaryOperator::Pow;

			auto zero = awst::makeZero(_loc);

			auto cond = awst::makeNumericCompare(e->right, awst::NumericComparison::Eq, std::move(zero), _loc);

			auto one = awst::makeOne(_loc);

			return awst::makeConditional(
				std::move(cond), std::move(one), e, awst::WType::uint64Type(), _loc);
		}
		case Token::SHL: case Token::AssignShl: e->op = awst::UInt64BinaryOperator::LShift; break;
		case Token::SHR: case Token::AssignShr: case Token::SAR: case Token::AssignSar: e->op = awst::UInt64BinaryOperator::RShift; break;
		case Token::BitOr: case Token::AssignBitOr: e->op = awst::UInt64BinaryOperator::BitOr; break;
		case Token::BitXor: case Token::AssignBitXor: e->op = awst::UInt64BinaryOperator::BitXor; break;
		case Token::BitAnd: case Token::AssignBitAnd: e->op = awst::UInt64BinaryOperator::BitAnd; break;
		default: e->op = awst::UInt64BinaryOperator::Add; break;
		}
		return e;
	}
}

} // namespace puyasol::builder::eb
