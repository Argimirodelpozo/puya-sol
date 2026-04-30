#include "builder/sol-eb/BinaryOpBuilder.h"

#include "builder/sol-eb/BuilderContext.h"
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
	BuilderContext& _ctx,
	solidity::frontend::Token _op,
	std::shared_ptr<awst::Expression> _left,
	std::shared_ptr<awst::Expression> _right,
	awst::WType const* _resultType,
	awst::SourceLocation const& _loc
)
{
	using Token = solidity::frontend::Token;

	// Helper to coerce bytes[N] operands to a numeric type when used in numeric context.
	// For bytes[N] where N > 8 — or bytes of unknown length (e.g. keccak256
	// output, which is a 32-byte digest but comes back typed as `bytes`) —
	// promotes to biguint via ReinterpretCast. btoi only handles ≤8 bytes
	// and fails at runtime for anything longer, so unknown-length bytes
	// must take the biguint path.
	// For fixed bytes[N≤8], uses btoi → uint64.
	auto coerceBytesToUint = [&](std::shared_ptr<awst::Expression>& operand) {
		if (operand->wtype && operand->wtype->kind() == awst::WTypeKind::Bytes)
		{
			auto const* bytesWType = dynamic_cast<awst::BytesWType const*>(operand->wtype);
			bool knownSmall =
				bytesWType && bytesWType->length().has_value() && *bytesWType->length() <= 8;
			if (!knownSmall)
			{
				auto cast = awst::makeReinterpretCast(std::move(operand), awst::WType::biguintType(), _loc);
				operand = std::move(cast);
				return;
			}
			auto expr = std::move(operand);
			if (expr->wtype != awst::WType::bytesType())
			{
				auto toBytes = awst::makeReinterpretCast(std::move(expr), awst::WType::bytesType(), _loc);
				expr = std::move(toBytes);
			}
			auto btoi = awst::makeIntrinsicCall("btoi", awst::WType::uint64Type(), _loc);
			btoi->stackArgs.push_back(std::move(expr));
			operand = std::move(btoi);
		}
	};

	// Auto-coerce bytes[N] operands to uint64 when the other operand is numeric
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

	// Helper to promote uint64 to biguint
	auto promoteToBigUInt = [&](std::shared_ptr<awst::Expression>& operand) {
		if (operand->wtype == awst::WType::uint64Type())
		{
			// For integer constants, use IntegerConstant(biguint) directly
			// to avoid itob(0) producing 8 zero bytes vs biguint(0) = empty bytes.
			if (auto const* intConst = dynamic_cast<awst::IntegerConstant const*>(operand.get()))
			{
				auto bigConst = awst::makeIntegerConstant(intConst->value, _loc, awst::WType::biguintType());
				operand = std::move(bigConst);
				return;
			}

			auto itob = awst::makeIntrinsicCall("itob", awst::WType::bytesType(), _loc);
			itob->stackArgs.push_back(std::move(operand));

			auto cast = awst::makeReinterpretCast(std::move(itob), awst::WType::biguintType(), _loc);
			operand = std::move(cast);
		}
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
		// For bytes-backed types (account, bytes, bytes[N], string), use BytesComparisonExpression
		bool isBytesBacked = _left->wtype == awst::WType::accountType()
			|| (_left->wtype && _left->wtype->kind() == awst::WTypeKind::Bytes)
			|| _left->wtype == awst::WType::stringType();

		if (isBytesBacked && (_op == Token::Equal || _op == Token::NotEqual))
		{
			// Coerce both sides to bytes if they differ
			if (_left->wtype != _right->wtype)
			{
				auto castToBytes = [&](std::shared_ptr<awst::Expression>& expr) {
					if (expr->wtype != awst::WType::bytesType())
					{
						auto cast = awst::makeReinterpretCast(std::move(expr), awst::WType::bytesType(), _loc);
						expr = std::move(cast);
					}
				};
				castToBytes(_left);
				castToBytes(_right);
			}
			auto e = std::make_shared<awst::BytesComparisonExpression>();
			e->sourceLocation = _loc;
			e->wtype = awst::WType::boolType();
			e->lhs = std::move(_left);
			e->rhs = std::move(_right);
			e->op = (_op == Token::Equal) ? awst::EqualityComparison::Eq : awst::EqualityComparison::Ne;
			return e;
		}

		// Bytes ordering comparisons use AVM intrinsics (b<, b>, b<=, b>=)
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

		// Promote if mixed uint64/biguint
		if (isBigUInt(_left->wtype) != isBigUInt(_right->wtype))
		{
			promoteToBigUInt(_left);
			promoteToBigUInt(_right);
		}

		auto e = std::make_shared<awst::NumericComparisonExpression>();
		e->sourceLocation = _loc;
		e->wtype = awst::WType::boolType();
		e->lhs = std::move(_left);
		e->rhs = std::move(_right);
		switch (_op)
		{
		case Token::Equal: e->op = awst::NumericComparison::Eq; break;
		case Token::NotEqual: e->op = awst::NumericComparison::Ne; break;
		case Token::LessThan: e->op = awst::NumericComparison::Lt; break;
		case Token::LessThanOrEqual: e->op = awst::NumericComparison::Lte; break;
		case Token::GreaterThan: e->op = awst::NumericComparison::Gt; break;
		case Token::GreaterThanOrEqual: e->op = awst::NumericComparison::Gte; break;
		default: break;
		}
		return e;
	}

	// Boolean operations
	case Token::And:
	{
		auto e = std::make_shared<awst::BooleanBinaryOperation>();
		e->sourceLocation = _loc;
		e->wtype = awst::WType::boolType();
		e->left = std::move(_left);
		e->op = awst::BinaryBooleanOperator::And;
		e->right = std::move(_right);
		return e;
	}
	case Token::Or:
	{
		auto e = std::make_shared<awst::BooleanBinaryOperation>();
		e->sourceLocation = _loc;
		e->wtype = awst::WType::boolType();
		e->left = std::move(_left);
		e->op = awst::BinaryBooleanOperator::Or;
		e->right = std::move(_right);
		return e;
	}

	default:
		break;
	}

	// Bytes bitwise operations (b|, b&, b^) — for bytes[N] types like bytes32/bytes4
	{
		bool leftIsBytesKind = _left->wtype && _left->wtype->kind() == awst::WTypeKind::Bytes;
		bool rightIsBytesKind = _right->wtype && _right->wtype->kind() == awst::WTypeKind::Bytes;
		bool isBitwiseOp = (_op == Token::BitOr || _op == Token::AssignBitOr
			|| _op == Token::BitXor || _op == Token::AssignBitXor
			|| _op == Token::BitAnd || _op == Token::AssignBitAnd);

		if ((leftIsBytesKind || rightIsBytesKind) && isBitwiseOp)
		{
			auto e = std::make_shared<awst::BytesBinaryOperation>();
			e->sourceLocation = _loc;
			e->wtype = awst::WType::bytesType();
			e->left = std::move(_left);
			e->right = std::move(_right);

			switch (_op)
			{
			case Token::BitOr: case Token::AssignBitOr: e->op = awst::BytesBinaryOperator::BitOr; break;
			case Token::BitXor: case Token::AssignBitXor: e->op = awst::BytesBinaryOperator::BitXor; break;
			case Token::BitAnd: case Token::AssignBitAnd: e->op = awst::BytesBinaryOperator::BitAnd; break;
			default: e->op = awst::BytesBinaryOperator::BitOr; break;
			}
			return e;
		}
	}

	// Arithmetic/bitwise operations — choose uint64 vs biguint
	if (isBigUInt(_resultType) || isBigUInt(_left->wtype) || isBigUInt(_right->wtype))
	{
		promoteToBigUInt(_left);

		auto e = std::make_shared<awst::BigUIntBinaryOperation>();
		e->sourceLocation = _loc;
		e->wtype = awst::WType::biguintType();

		// BigUInt doesn't have native shift ops — convert x<<n to x*(2^n), x>>n to x/(2^n)
		// Construct 2^n using setbit(bzero(32), 255-n, 1) since AVM has no bexp opcode
		// Note: _right (shift amount) must stay uint64 — do NOT promote it before this block
		if (_op == Token::SHL || _op == Token::AssignShl
			|| _op == Token::SHR || _op == Token::AssignShr
			|| _op == Token::SAR || _op == Token::AssignSar)
		{
			auto shiftAmt = TypeCoercion::implicitNumericCast(std::move(_right), awst::WType::uint64Type(), _loc);

			// bzero(32) — 256-bit zero buffer
			auto thirtyTwo = awst::makeIntegerConstant("32", _loc);

			auto bzero = awst::makeIntrinsicCall("bzero", awst::WType::bytesType(), _loc);
			bzero->stackArgs.push_back(std::move(thirtyTwo));

			// 255 - n: setbit uses MSB-first ordering, so bit (255-n) = 2^n
			auto twoFiftyFive = awst::makeIntegerConstant("255", _loc);

			auto bitIdx = awst::makeUInt64BinOp(std::move(twoFiftyFive), awst::UInt64BinaryOperator::Sub, std::move(shiftAmt), _loc);

			// setbit(bzero(32), 255-n, 1) → bytes with only bit n set
			auto one = awst::makeIntegerConstant("1", _loc);

			auto setbit = awst::makeIntrinsicCall("setbit", awst::WType::bytesType(), _loc);
			setbit->stackArgs.push_back(std::move(bzero));
			setbit->stackArgs.push_back(std::move(bitIdx));
			setbit->stackArgs.push_back(std::move(one));

			// Cast bytes → biguint
			auto castToBigUInt = awst::makeReinterpretCast(std::move(setbit), awst::WType::biguintType(), _loc);

			e->left = std::move(_left);
			e->right = std::move(castToBigUInt);
			e->op = (_op == Token::SHL || _op == Token::AssignShl)
				? awst::BigUIntBinaryOperator::Mult
				: awst::BigUIntBinaryOperator::FloorDiv;
			return e;
		}

		// For non-shift ops, promote right operand to biguint now
		promoteToBigUInt(_right);

		if (_op == Token::Sub || _op == Token::AssignSub)
		{
			// Checked subtraction: assert a >= b before wrapping
			if (!_ctx.inUncheckedBlock)
			{
				auto cmp = awst::makeNumericCompare(_left, awst::NumericComparison::Gte, _right, _loc);

				auto assertStmt = awst::makeExpressionStatement(awst::makeAssert(std::move(cmp), _loc, "underflow"), _loc);
				_ctx.prePendingStatements.push_back(std::move(assertStmt));
			}

			// Biguint subtraction needs wrapping mod 2^256 to avoid AVM underflow.
			// Pattern: (a + 2^256 - b) % 2^256
			auto pow256 = awst::makeIntegerConstant(kPow2_256, _loc, awst::WType::biguintType());

			auto addPow = awst::makeBigUIntBinOp(std::move(_left), awst::BigUIntBinaryOperator::Add, pow256, _loc);

			auto diff = awst::makeBigUIntBinOp(std::move(addPow), awst::BigUIntBinaryOperator::Sub, std::move(_right), _loc);

			auto pow256b = awst::makeIntegerConstant(kPow2_256, _loc, awst::WType::biguintType());

			auto mod = awst::makeBigUIntBinOp(std::move(diff), awst::BigUIntBinaryOperator::Mod, std::move(pow256b), _loc);
			return mod;
		}


		// BigUInt exponentiation: AVM has no biguint exp opcode, so build a
		// square-and-multiply loop emitted via prePendingStatements.
		if (_op == Token::Exp)
		{
			static int expCounter = 0;
			int id = expCounter++;
			std::string resultVar = "__biguint_exp_result_" + std::to_string(id);
			std::string baseVar = "__biguint_exp_base_" + std::to_string(id);
			std::string expVar = "__biguint_exp_exp_" + std::to_string(id);

			auto makeVar = [&](std::string const& name) -> std::shared_ptr<awst::VarExpression>
			{
				auto v = awst::makeVarExpression(name, awst::WType::biguintType(), _loc);
				return v;
			};
			auto makeConst = [&](std::string const& value) -> std::shared_ptr<awst::IntegerConstant>
			{
				auto c = awst::makeIntegerConstant(value, _loc, awst::WType::biguintType());
				return c;
			};
			auto makeAssign = [&](
				std::string const& target,
				std::shared_ptr<awst::Expression> value
			) -> std::shared_ptr<awst::AssignmentStatement>
			{
				auto assign = awst::makeAssignmentStatement(makeVar(target), std::move(value), _loc);
				return assign;
			};
			auto makeBinOp = [&](
				std::shared_ptr<awst::Expression> lhs,
				awst::BigUIntBinaryOperator op,
				std::shared_ptr<awst::Expression> rhs
			) -> std::shared_ptr<awst::BigUIntBinaryOperation>
			{
				auto bin = awst::makeBigUIntBinOp(std::move(lhs), op, std::move(rhs), _loc);
				return bin;
			};

			// Ensure both operands are biguint (they may be uint64)
			auto baseExpr = TypeCoercion::implicitNumericCast(std::move(_left), awst::WType::biguintType(), _loc);
			auto expExpr = TypeCoercion::implicitNumericCast(std::move(_right), awst::WType::biguintType(), _loc);

			// __biguint_exp_result = 1
			_ctx.prePendingStatements.push_back(makeAssign(resultVar, makeConst("1")));
			// __biguint_exp_base = base
			_ctx.prePendingStatements.push_back(makeAssign(baseVar, std::move(baseExpr)));
			// __biguint_exp_exp = exp
			_ctx.prePendingStatements.push_back(makeAssign(expVar, std::move(expExpr)));

			// while __biguint_exp_exp > 0:
			auto loop = std::make_shared<awst::WhileLoop>();
			loop->sourceLocation = _loc;
			{
				auto cond = awst::makeNumericCompare(makeVar(expVar), awst::NumericComparison::Gt, makeConst("0"), _loc);
				loop->condition = std::move(cond);
			}

			auto body = std::make_shared<awst::Block>();
			body->sourceLocation = _loc;

			// In unchecked mode, Solidity wraps exponentiation modulo 2^256
			// so that huge exponents (e.g. 2**1113) don't overflow biguint.
			// Take each intermediate result mod 2^256 inside the loop.
			bool const wrapMod = _ctx.inUncheckedBlock;
			auto wrapMod256 = [&](std::shared_ptr<awst::Expression> v)
				-> std::shared_ptr<awst::Expression>
			{
				if (!wrapMod) return v;
				auto mod = awst::makeBigUIntBinOp(std::move(v), awst::BigUIntBinaryOperator::Mod, makeConst(kPow2_256), _loc);
				return mod;
			};

			// if exp & 1 != 0: result = result * base
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

			// exp = exp / 2
			body->body.push_back(makeAssign(expVar,
				makeBinOp(makeVar(expVar), awst::BigUIntBinaryOperator::FloorDiv, makeConst("2"))));

			// base = base * base (wrapped mod 2^256 in unchecked mode)
			{
				std::shared_ptr<awst::Expression> baseSq =
					makeBinOp(makeVar(baseVar), awst::BigUIntBinaryOperator::Mult, makeVar(baseVar));
				baseSq = wrapMod256(std::move(baseSq));
				body->body.push_back(makeAssign(baseVar, std::move(baseSq)));
			}

			loop->loopBody = std::move(body);
			_ctx.prePendingStatements.push_back(std::move(loop));

			return makeVar(resultVar);
		}

		e->left = std::move(_left);
		e->right = std::move(_right);

		switch (_op)
		{
		case Token::Add: case Token::AssignAdd: e->op = awst::BigUIntBinaryOperator::Add; break;
		case Token::Mul: case Token::AssignMul: e->op = awst::BigUIntBinaryOperator::Mult; break;
		case Token::Div: case Token::AssignDiv: e->op = awst::BigUIntBinaryOperator::FloorDiv; break;
		case Token::Mod: case Token::AssignMod: e->op = awst::BigUIntBinaryOperator::Mod; break;
		case Token::BitOr: case Token::AssignBitOr: e->op = awst::BigUIntBinaryOperator::BitOr; break;
		case Token::BitXor: case Token::AssignBitXor: e->op = awst::BigUIntBinaryOperator::BitXor; break;
		case Token::BitAnd: case Token::AssignBitAnd: e->op = awst::BigUIntBinaryOperator::BitAnd; break;
		default: e->op = awst::BigUIntBinaryOperator::Add; break;
		}

		// In unchecked blocks, arithmetic must wrap mod 2^256 (EVM semantics).
		// AVM biguint is arbitrary-precision; without truncation, results can
		// exceed 256 bits and break subsequent operations.
		if (_ctx.inUncheckedBlock
			&& (_op == Token::Add || _op == Token::AssignAdd
				|| _op == Token::Sub || _op == Token::AssignSub
				|| _op == Token::Mul || _op == Token::AssignMul))
		{
			auto pow256 = awst::makeIntegerConstant(kPow2_256, _loc, awst::WType::biguintType());

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
			// AVM `exp` opcode asserts on 0^0. Solidity defines 0**0 = 1.
			// Wrap: y == 0 ? 1 : x ** y
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
			return ternary;
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
