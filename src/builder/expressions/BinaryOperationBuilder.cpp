/// @file BinaryOperationBuilder.cpp
/// Handles binary operations (+, -, *, /, %, comparisons, shifts, etc.).

#include "builder/expressions/ExpressionBuilder.h"
#include "builder/storage/StorageMapper.h"
#include "Logger.h"

#include <libsolidity/ast/TypeProvider.h>

#include <algorithm>
#include <sstream>

namespace puyasol::builder
{

std::shared_ptr<awst::Expression> ExpressionBuilder::buildBinaryOp(
	solidity::frontend::Token _op,
	std::shared_ptr<awst::Expression> _left,
	std::shared_ptr<awst::Expression> _right,
	awst::WType const* _resultType,
	awst::SourceLocation const& _loc
)
{
	using Token = solidity::frontend::Token;

	// Helper to coerce bytes[N] operands to a numeric type when used in numeric context.
	// For bytes[N] where N > 8, promotes to biguint (btoi only handles ≤8 bytes).
	// For smaller bytes, uses btoi → uint64.
	auto coerceBytesToUint = [&](std::shared_ptr<awst::Expression>& operand) {
		if (operand->wtype && operand->wtype->kind() == awst::WTypeKind::Bytes)
		{
			auto const* bytesWType = dynamic_cast<awst::BytesWType const*>(operand->wtype);
			if (bytesWType && bytesWType->length().has_value() && *bytesWType->length() > 8)
			{
				// bytes[N>8] → biguint via ReinterpretCast (btoi can't handle >8 bytes)
				auto cast = std::make_shared<awst::ReinterpretCast>();
				cast->sourceLocation = _loc;
				cast->wtype = awst::WType::biguintType();
				cast->expr = std::move(operand);
				operand = std::move(cast);
				return;
			}
			// bytes[N≤8] or unsized bytes → bytes → btoi → uint64
			auto expr = std::move(operand);
			if (expr->wtype != awst::WType::bytesType())
			{
				auto toBytes = std::make_shared<awst::ReinterpretCast>();
				toBytes->sourceLocation = _loc;
				toBytes->wtype = awst::WType::bytesType();
				toBytes->expr = std::move(expr);
				expr = std::move(toBytes);
			}
			auto btoi = std::make_shared<awst::IntrinsicCall>();
			btoi->sourceLocation = _loc;
			btoi->wtype = awst::WType::uint64Type();
			btoi->opCode = "btoi";
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
				auto bigConst = std::make_shared<awst::IntegerConstant>();
				bigConst->sourceLocation = _loc;
				bigConst->wtype = awst::WType::biguintType();
				bigConst->value = intConst->value;
				operand = std::move(bigConst);
				return;
			}

			auto itob = std::make_shared<awst::IntrinsicCall>();
			itob->sourceLocation = _loc;
			itob->wtype = awst::WType::bytesType();
			itob->opCode = "itob";
			itob->stackArgs.push_back(std::move(operand));

			auto cast = std::make_shared<awst::ReinterpretCast>();
			cast->sourceLocation = _loc;
			cast->wtype = awst::WType::biguintType();
			cast->expr = std::move(itob);
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
			// Coerce both sides to the same wtype (bytes) if they differ
			auto coerceToBytes = [&](std::shared_ptr<awst::Expression>& expr) {
				if (expr->wtype != awst::WType::bytesType()
					&& expr->wtype != awst::WType::accountType())
				{
					auto cast = std::make_shared<awst::ReinterpretCast>();
					cast->sourceLocation = _loc;
					cast->wtype = awst::WType::bytesType();
					cast->expr = std::move(expr);
					expr = std::move(cast);
				}
			};
			if (_left->wtype != _right->wtype)
			{
				coerceToBytes(_left);
				coerceToBytes(_right);
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
				auto e = std::make_shared<awst::IntrinsicCall>();
				e->sourceLocation = _loc;
				e->wtype = awst::WType::boolType();
				e->opCode = std::move(opCode);
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
			auto shiftAmt = implicitNumericCast(std::move(_right), awst::WType::uint64Type(), _loc);

			// bzero(32) — 256-bit zero buffer
			auto thirtyTwo = std::make_shared<awst::IntegerConstant>();
			thirtyTwo->sourceLocation = _loc;
			thirtyTwo->wtype = awst::WType::uint64Type();
			thirtyTwo->value = "32";

			auto bzero = std::make_shared<awst::IntrinsicCall>();
			bzero->sourceLocation = _loc;
			bzero->wtype = awst::WType::bytesType();
			bzero->opCode = "bzero";
			bzero->stackArgs.push_back(std::move(thirtyTwo));

			// 255 - n: setbit uses MSB-first ordering, so bit (255-n) = 2^n
			auto twoFiftyFive = std::make_shared<awst::IntegerConstant>();
			twoFiftyFive->sourceLocation = _loc;
			twoFiftyFive->wtype = awst::WType::uint64Type();
			twoFiftyFive->value = "255";

			auto bitIdx = std::make_shared<awst::UInt64BinaryOperation>();
			bitIdx->sourceLocation = _loc;
			bitIdx->wtype = awst::WType::uint64Type();
			bitIdx->left = std::move(twoFiftyFive);
			bitIdx->right = std::move(shiftAmt);
			bitIdx->op = awst::UInt64BinaryOperator::Sub;

			// setbit(bzero(32), 255-n, 1) → bytes with only bit n set
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

			// Cast bytes → biguint
			auto castToBigUInt = std::make_shared<awst::ReinterpretCast>();
			castToBigUInt->sourceLocation = _loc;
			castToBigUInt->wtype = awst::WType::biguintType();
			castToBigUInt->expr = std::move(setbit);

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
			if (!m_inUncheckedBlock)
			{
				auto cmp = std::make_shared<awst::NumericComparisonExpression>();
				cmp->sourceLocation = _loc;
				cmp->wtype = awst::WType::boolType();
				cmp->lhs = _left;   // shared ref, OK since BigUInt is immutable
				cmp->op = awst::NumericComparison::Gte;
				cmp->rhs = _right;  // shared ref

				auto assertStmt = std::make_shared<awst::ExpressionStatement>();
				assertStmt->sourceLocation = _loc;
				auto assertExpr = std::make_shared<awst::AssertExpression>();
				assertExpr->sourceLocation = _loc;
				assertExpr->wtype = awst::WType::voidType();
				assertExpr->condition = std::move(cmp);
				assertExpr->errorMessage = "underflow";
				assertStmt->expr = std::move(assertExpr);
				m_prePendingStatements.push_back(std::move(assertStmt));
			}

			// Biguint subtraction needs wrapping mod 2^256 to avoid AVM underflow.
			// Pattern: (a + 2^256 - b) % 2^256
			auto pow256 = std::make_shared<awst::IntegerConstant>();
			pow256->sourceLocation = _loc;
			pow256->wtype = awst::WType::biguintType();
			pow256->value = "115792089237316195423570985008687907853269984665640564039457584007913129639936";

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

			auto pow256b = std::make_shared<awst::IntegerConstant>();
			pow256b->sourceLocation = _loc;
			pow256b->wtype = awst::WType::biguintType();
			pow256b->value = "115792089237316195423570985008687907853269984665640564039457584007913129639936";

			auto mod = std::make_shared<awst::BigUIntBinaryOperation>();
			mod->sourceLocation = _loc;
			mod->wtype = awst::WType::biguintType();
			mod->left = std::move(diff);
			mod->op = awst::BigUIntBinaryOperator::Mod;
			mod->right = std::move(pow256b);
			return mod;
		}


		// BigUInt exponentiation: AVM has no biguint exp opcode, so build a
		// square-and-multiply loop emitted via m_pendingStatements.
		if (_op == Token::Exp)
		{
			static int expCounter = 0;
			int id = expCounter++;
			std::string resultVar = "__biguint_exp_result_" + std::to_string(id);
			std::string baseVar = "__biguint_exp_base_" + std::to_string(id);
			std::string expVar = "__biguint_exp_exp_" + std::to_string(id);

			auto makeVar = [&](std::string const& name) -> std::shared_ptr<awst::VarExpression>
			{
				auto v = std::make_shared<awst::VarExpression>();
				v->sourceLocation = _loc;
				v->name = name;
				v->wtype = awst::WType::biguintType();
				return v;
			};
			auto makeConst = [&](std::string const& value) -> std::shared_ptr<awst::IntegerConstant>
			{
				auto c = std::make_shared<awst::IntegerConstant>();
				c->sourceLocation = _loc;
				c->wtype = awst::WType::biguintType();
				c->value = value;
				return c;
			};
			auto makeAssign = [&](
				std::string const& target,
				std::shared_ptr<awst::Expression> value
			) -> std::shared_ptr<awst::AssignmentStatement>
			{
				auto assign = std::make_shared<awst::AssignmentStatement>();
				assign->sourceLocation = _loc;
				assign->target = makeVar(target);
				assign->value = std::move(value);
				return assign;
			};
			auto makeBinOp = [&](
				std::shared_ptr<awst::Expression> lhs,
				awst::BigUIntBinaryOperator op,
				std::shared_ptr<awst::Expression> rhs
			) -> std::shared_ptr<awst::BigUIntBinaryOperation>
			{
				auto bin = std::make_shared<awst::BigUIntBinaryOperation>();
				bin->sourceLocation = _loc;
				bin->wtype = awst::WType::biguintType();
				bin->left = std::move(lhs);
				bin->op = op;
				bin->right = std::move(rhs);
				return bin;
			};

			// Ensure both operands are biguint (they may be uint64)
			auto baseExpr = implicitNumericCast(std::move(_left), awst::WType::biguintType(), _loc);
			auto expExpr = implicitNumericCast(std::move(_right), awst::WType::biguintType(), _loc);

			// __biguint_exp_result = 1
			m_prePendingStatements.push_back(makeAssign(resultVar, makeConst("1")));
			// __biguint_exp_base = base
			m_prePendingStatements.push_back(makeAssign(baseVar, std::move(baseExpr)));
			// __biguint_exp_exp = exp
			m_prePendingStatements.push_back(makeAssign(expVar, std::move(expExpr)));

			// while __biguint_exp_exp > 0:
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

				auto product = makeBinOp(makeVar(resultVar), awst::BigUIntBinaryOperator::Mult, makeVar(baseVar));

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

			// base = base * base
			body->body.push_back(makeAssign(baseVar,
				makeBinOp(makeVar(baseVar), awst::BigUIntBinaryOperator::Mult, makeVar(baseVar))));

			loop->loopBody = std::move(body);
			m_prePendingStatements.push_back(std::move(loop));

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
		if (m_inUncheckedBlock
			&& (_op == Token::Add || _op == Token::AssignAdd
				|| _op == Token::Sub || _op == Token::AssignSub
				|| _op == Token::Mul || _op == Token::AssignMul))
		{
			auto pow256 = std::make_shared<awst::IntegerConstant>();
			pow256->sourceLocation = _loc;
			pow256->wtype = awst::WType::biguintType();
			pow256->value = "115792089237316195423570985008687907853269984665640564039457584007913129639936";

			auto mod = std::make_shared<awst::BigUIntBinaryOperation>();
			mod->sourceLocation = _loc;
			mod->wtype = awst::WType::biguintType();
			mod->left = e;
			mod->op = awst::BigUIntBinaryOperator::Mod;
			mod->right = std::move(pow256);
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

			auto zero = std::make_shared<awst::IntegerConstant>();
			zero->sourceLocation = _loc;
			zero->wtype = awst::WType::uint64Type();
			zero->value = "0";

			auto cond = std::make_shared<awst::NumericComparisonExpression>();
			cond->sourceLocation = _loc;
			cond->wtype = awst::WType::boolType();
			cond->lhs = e->right; // y (shared ref)
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

bool ExpressionBuilder::visit(solidity::frontend::BinaryOperation const& _node)
{
	auto loc = makeLoc(_node.location());

	// Check for user-defined operator overloading (e.g. `using {add as +} for Fr`)
	if (auto const* userFunc = *_node.annotation().userDefinedFunction)
	{
		// Look up the subroutine ID for this free/library function
		std::string subroutineId;
		auto it = m_freeFunctionById.find(userFunc->id());
		if (it != m_freeFunctionById.end())
			subroutineId = it->second;
		else
		{
			// Try library function lookup — prefer AST ID for overloaded functions
			auto byId = m_freeFunctionById.find(userFunc->id());
			if (byId != m_freeFunctionById.end())
				subroutineId = byId->second;
			else
			{
				auto const* scope = userFunc->scope();
				auto const* libContract = dynamic_cast<solidity::frontend::ContractDefinition const*>(scope);
				if (libContract && libContract->isLibrary())
				{
					std::string qualifiedName = libContract->name() + "." + userFunc->name();
					auto libIt = m_libraryFunctionIds.find(qualifiedName);
					if (libIt != m_libraryFunctionIds.end())
						subroutineId = libIt->second;
				}
			}
			if (subroutineId.empty())
				subroutineId = m_sourceFile + "." + userFunc->name();
		}

		auto left = build(_node.leftExpression());
		auto right = build(_node.rightExpression());
		auto* resultType = m_typeMapper.map(_node.annotation().type);

		auto call = std::make_shared<awst::SubroutineCallExpression>();
		call->sourceLocation = loc;
		call->wtype = resultType;
		call->target = awst::SubroutineID{subroutineId};

		awst::CallArg argA;
		argA.name = userFunc->parameters()[0]->name();
		argA.value = std::move(left);
		call->args.push_back(std::move(argA));

		awst::CallArg argB;
		argB.name = userFunc->parameters()[1]->name();
		argB.value = std::move(right);
		call->args.push_back(std::move(argB));

		push(std::move(call));
		return false;
	}

	// Check if the Solidity type checker resolved this to a compile-time constant
	// (e.g., 2**136, MODULUS - 1, 1 << 68). This handles all constant binary ops
	// including ** and << for biguint which AWST doesn't support as runtime ops.
	if (auto const* ratType = dynamic_cast<solidity::frontend::RationalNumberType const*>(
		_node.annotation().type))
	{
		if (!ratType->isFractional())
		{
			auto* resultType = m_typeMapper.map(_node.annotation().type);
			auto e = std::make_shared<awst::IntegerConstant>();
			e->sourceLocation = loc;
			e->wtype = resultType;
			e->value = ratType->literalValue(nullptr).str();
			push(e);
			return false;
		}
	}

	auto left = build(_node.leftExpression());
	auto right = build(_node.rightExpression());
	auto* resultType = m_typeMapper.map(_node.annotation().type);

	// For signed integer comparisons (<, >, <=, >=), we need signed semantics.
	// AVM only has unsigned comparison, so we XOR both operands with the sign bit
	// (2^63 for uint64, 2^255 for biguint) to convert signed ordering to unsigned ordering.
	using Token = solidity::frontend::Token;
	auto op = _node.getOperator();
	if (op == Token::LessThan || op == Token::LessThanOrEqual ||
		op == Token::GreaterThan || op == Token::GreaterThanOrEqual)
	{
		// Check if the operands' Solidity types are signed integers
		auto const* leftSolType = _node.leftExpression().annotation().type;
		auto const* rightSolType = _node.rightExpression().annotation().type;
		auto const* leftInt = dynamic_cast<solidity::frontend::IntegerType const*>(leftSolType);
		auto const* rightInt = dynamic_cast<solidity::frontend::IntegerType const*>(rightSolType);
		bool isSigned = (leftInt && leftInt->isSigned()) || (rightInt && rightInt->isSigned());

		if (isSigned && left->wtype == awst::WType::uint64Type()
			&& right->wtype == awst::WType::uint64Type())
		{
			// XOR both with 2^63 to make signed comparison work with unsigned </>
			auto signBit = std::make_shared<awst::IntegerConstant>();
			signBit->sourceLocation = loc;
			signBit->wtype = awst::WType::uint64Type();
			signBit->value = "9223372036854775808"; // 2^63

			auto xorLeft = std::make_shared<awst::UInt64BinaryOperation>();
			xorLeft->sourceLocation = loc;
			xorLeft->wtype = awst::WType::uint64Type();
			xorLeft->left = std::move(left);
			xorLeft->op = awst::UInt64BinaryOperator::BitXor;
			xorLeft->right = signBit;
			left = std::move(xorLeft);

			auto signBit2 = std::make_shared<awst::IntegerConstant>();
			signBit2->sourceLocation = loc;
			signBit2->wtype = awst::WType::uint64Type();
			signBit2->value = "9223372036854775808";

			auto xorRight = std::make_shared<awst::UInt64BinaryOperation>();
			xorRight->sourceLocation = loc;
			xorRight->wtype = awst::WType::uint64Type();
			xorRight->left = std::move(right);
			xorRight->op = awst::UInt64BinaryOperator::BitXor;
			xorRight->right = std::move(signBit2);
			right = std::move(xorRight);
		}
		else if (isSigned && (isBigUInt(left->wtype) || isBigUInt(right->wtype)))
		{
			// Promote uint64 operand to biguint if needed
			if (!isBigUInt(left->wtype))
				left = implicitNumericCast(std::move(left), awst::WType::biguintType(), loc);
			if (!isBigUInt(right->wtype))
				right = implicitNumericCast(std::move(right), awst::WType::biguintType(), loc);
			// XOR both with 2^255 for biguint signed comparison
			solidity::u256 signBitVal = solidity::u256(1) << 255;
			auto signBit = std::make_shared<awst::IntegerConstant>();
			signBit->sourceLocation = loc;
			signBit->wtype = awst::WType::biguintType();
			signBit->value = signBitVal.str();

			auto xorLeft = std::make_shared<awst::BigUIntBinaryOperation>();
			xorLeft->sourceLocation = loc;
			xorLeft->wtype = awst::WType::biguintType();
			xorLeft->left = std::move(left);
			xorLeft->op = awst::BigUIntBinaryOperator::BitXor;
			xorLeft->right = signBit;
			left = std::move(xorLeft);

			auto signBit2 = std::make_shared<awst::IntegerConstant>();
			signBit2->sourceLocation = loc;
			signBit2->wtype = awst::WType::biguintType();
			signBit2->value = signBitVal.str();

			auto xorRight = std::make_shared<awst::BigUIntBinaryOperation>();
			xorRight->sourceLocation = loc;
			xorRight->wtype = awst::WType::biguintType();
			xorRight->left = std::move(right);
			xorRight->op = awst::BigUIntBinaryOperator::BitXor;
			xorRight->right = std::move(signBit2);
			right = std::move(xorRight);
		}
	}

	// Signed modulo/division: operate on absolute values, then apply sign
	{
		auto const* leftInt = dynamic_cast<solidity::frontend::IntegerType const*>(
			_node.leftExpression().annotation().type);
		auto const* rightInt = dynamic_cast<solidity::frontend::IntegerType const*>(
			_node.rightExpression().annotation().type);
		bool isSigned = (leftInt && leftInt->isSigned()) || (rightInt && rightInt->isSigned());
		auto op = _node.getOperator();
		bool isModOrDiv = (op == Token::Mod || op == Token::AssignMod
			|| op == Token::Div || op == Token::AssignDiv);

		if (isSigned && isModOrDiv && isBigUInt(left->wtype))
		{
			// Two's complement: negative if value >= 2^255
			// abs(x) = x < 2^255 ? x : 2^256 - x
			// neg(x) = 2^256 - x
			auto pow255 = std::make_shared<awst::IntegerConstant>();
			pow255->sourceLocation = loc;
			pow255->wtype = awst::WType::biguintType();
			pow255->value = "57896044618658097711785492504343953926634992332820282019728792003956564819968"; // 2^255

			auto pow256 = std::make_shared<awst::IntegerConstant>();
			pow256->sourceLocation = loc;
			pow256->wtype = awst::WType::biguintType();
			pow256->value = "115792089237316195423570985008687907853269984665640564039457584007913129639936"; // 2^256

			// isLeftNeg = left >= 2^255
			auto isLeftNeg = std::make_shared<awst::NumericComparisonExpression>();
			isLeftNeg->sourceLocation = loc;
			isLeftNeg->wtype = awst::WType::boolType();
			isLeftNeg->lhs = left;
			isLeftNeg->op = awst::NumericComparison::Gte;
			isLeftNeg->rhs = pow255;

			// absLeft = isLeftNeg ? (2^256 - left) : left
			auto negLeft = std::make_shared<awst::BigUIntBinaryOperation>();
			negLeft->sourceLocation = loc;
			negLeft->wtype = awst::WType::biguintType();
			negLeft->left = pow256;
			negLeft->op = awst::BigUIntBinaryOperator::Sub;
			negLeft->right = left;

			auto absLeft = std::make_shared<awst::ConditionalExpression>();
			absLeft->sourceLocation = loc;
			absLeft->wtype = awst::WType::biguintType();
			absLeft->condition = isLeftNeg;
			absLeft->trueExpr = std::move(negLeft);
			absLeft->falseExpr = left;

			// isRightNeg = right >= 2^255
			auto pow255_2 = std::make_shared<awst::IntegerConstant>();
			pow255_2->sourceLocation = loc;
			pow255_2->wtype = awst::WType::biguintType();
			pow255_2->value = "57896044618658097711785492504343953926634992332820282019728792003956564819968";

			auto isRightNeg = std::make_shared<awst::NumericComparisonExpression>();
			isRightNeg->sourceLocation = loc;
			isRightNeg->wtype = awst::WType::boolType();
			isRightNeg->lhs = right;
			isRightNeg->op = awst::NumericComparison::Gte;
			isRightNeg->rhs = std::move(pow255_2);

			auto pow256_2 = std::make_shared<awst::IntegerConstant>();
			pow256_2->sourceLocation = loc;
			pow256_2->wtype = awst::WType::biguintType();
			pow256_2->value = "115792089237316195423570985008687907853269984665640564039457584007913129639936";

			auto negRight = std::make_shared<awst::BigUIntBinaryOperation>();
			negRight->sourceLocation = loc;
			negRight->wtype = awst::WType::biguintType();
			negRight->left = std::move(pow256_2);
			negRight->op = awst::BigUIntBinaryOperator::Sub;
			negRight->right = right;

			auto absRight = std::make_shared<awst::ConditionalExpression>();
			absRight->sourceLocation = loc;
			absRight->wtype = awst::WType::biguintType();
			absRight->condition = isRightNeg;
			absRight->trueExpr = std::move(negRight);
			absRight->falseExpr = right;

			// Compute abs result
			awst::BigUIntBinaryOperator unsignedOp =
				(op == Token::Mod || op == Token::AssignMod)
				? awst::BigUIntBinaryOperator::Mod
				: awst::BigUIntBinaryOperator::FloorDiv;

			auto absResult = std::make_shared<awst::BigUIntBinaryOperation>();
			absResult->sourceLocation = loc;
			absResult->wtype = awst::WType::biguintType();
			absResult->left = std::move(absLeft);
			absResult->op = unsignedOp;
			absResult->right = std::move(absRight);

			// Apply sign:
			// mod: sign follows dividend (left)
			// div: sign is negative if signs differ
			auto pow256_3 = std::make_shared<awst::IntegerConstant>();
			pow256_3->sourceLocation = loc;
			pow256_3->wtype = awst::WType::biguintType();
			pow256_3->value = "115792089237316195423570985008687907853269984665640564039457584007913129639936";

			auto negResult = std::make_shared<awst::BigUIntBinaryOperation>();
			negResult->sourceLocation = loc;
			negResult->wtype = awst::WType::biguintType();
			negResult->left = std::move(pow256_3);
			negResult->op = awst::BigUIntBinaryOperator::Sub;
			negResult->right = absResult;

			std::shared_ptr<awst::Expression> shouldNegate;
			if (op == Token::Mod || op == Token::AssignMod)
			{
				// mod: negate if dividend was negative
				shouldNegate = isLeftNeg;
			}
			else
			{
				// div: negate if signs differ (XOR of sign bits)
				auto xorSigns = std::make_shared<awst::BooleanBinaryOperation>();
				xorSigns->sourceLocation = loc;
				xorSigns->wtype = awst::WType::boolType();
				xorSigns->left = isLeftNeg;
				xorSigns->op = awst::BinaryBooleanOperator::And;
				// Actually need XOR: (a && !b) || (!a && b)
				// Simpler: just check if isLeftNeg != isRightNeg
				auto notEqual = std::make_shared<awst::Not>();
				notEqual->sourceLocation = loc;
				notEqual->wtype = awst::WType::boolType();

				auto bothNeg = std::make_shared<awst::BooleanBinaryOperation>();
				bothNeg->sourceLocation = loc;
				bothNeg->wtype = awst::WType::boolType();
				bothNeg->left = isLeftNeg;
				bothNeg->op = awst::BinaryBooleanOperator::And;
				bothNeg->right = isRightNeg;

				auto eitherNeg = std::make_shared<awst::BooleanBinaryOperation>();
				eitherNeg->sourceLocation = loc;
				eitherNeg->wtype = awst::WType::boolType();
				eitherNeg->left = isLeftNeg;
				eitherNeg->op = awst::BinaryBooleanOperator::Or;
				eitherNeg->right = isRightNeg;

				// XOR = eitherNeg && !bothNeg
				auto notBothNeg = std::make_shared<awst::Not>();
				notBothNeg->sourceLocation = loc;
				notBothNeg->wtype = awst::WType::boolType();
				notBothNeg->expr = std::move(bothNeg);

				shouldNegate = std::make_shared<awst::BooleanBinaryOperation>();
				shouldNegate->sourceLocation = loc;
				shouldNegate->wtype = awst::WType::boolType();
				std::static_pointer_cast<awst::BooleanBinaryOperation>(shouldNegate)->left = std::move(eitherNeg);
				std::static_pointer_cast<awst::BooleanBinaryOperation>(shouldNegate)->op = awst::BinaryBooleanOperator::And;
				std::static_pointer_cast<awst::BooleanBinaryOperation>(shouldNegate)->right = std::move(notBothNeg);
			}

			// result = shouldNegate ? negResult : absResult
			// But only if absResult != 0 (negating 0 gives 2^256 which is wrong)
			auto zero = std::make_shared<awst::IntegerConstant>();
			zero->sourceLocation = loc;
			zero->wtype = awst::WType::biguintType();
			zero->value = "0";

			auto isZero = std::make_shared<awst::NumericComparisonExpression>();
			isZero->sourceLocation = loc;
			isZero->wtype = awst::WType::boolType();
			isZero->lhs = absResult;
			isZero->op = awst::NumericComparison::Eq;
			isZero->rhs = std::move(zero);

			auto notZero = std::make_shared<awst::Not>();
			notZero->sourceLocation = loc;
			notZero->wtype = awst::WType::boolType();
			notZero->expr = std::move(isZero);

			auto shouldNegateAndNonZero = std::make_shared<awst::BooleanBinaryOperation>();
			shouldNegateAndNonZero->sourceLocation = loc;
			shouldNegateAndNonZero->wtype = awst::WType::boolType();
			shouldNegateAndNonZero->left = std::move(shouldNegate);
			shouldNegateAndNonZero->op = awst::BinaryBooleanOperator::And;
			shouldNegateAndNonZero->right = std::move(notZero);

			auto signedResult = std::make_shared<awst::ConditionalExpression>();
			signedResult->sourceLocation = loc;
			signedResult->wtype = awst::WType::biguintType();
			signedResult->condition = std::move(shouldNegateAndNonZero);
			signedResult->trueExpr = std::move(negResult);
			signedResult->falseExpr = std::move(absResult);

			push(std::move(signedResult));
			return false;
		}
	}

	auto result = buildBinaryOp(_node.getOperator(), std::move(left), std::move(right), resultType, loc);

	// Unchecked wrapping for uint64 results:
	// AVM uint64 doesn't naturally wrap — mask to Solidity bit width.
	// result = result & ((1 << bits) - 1)
	if (m_inUncheckedBlock && result->wtype == awst::WType::uint64Type())
	{
		auto const* solType = dynamic_cast<solidity::frontend::IntegerType const*>(
			_node.annotation().type);
		if (solType && !solType->isSigned())
		{
			unsigned bits = solType->numBits();
			auto checkOp = _node.getOperator();
			bool needsWrap = (checkOp == Token::Add || checkOp == Token::AssignAdd
				|| checkOp == Token::Sub || checkOp == Token::AssignSub
				|| checkOp == Token::Mul || checkOp == Token::AssignMul);

			if (needsWrap && bits < 64)
			{
				// Mask: result % (1 << bits)
				uint64_t modVal = uint64_t(1) << bits;
				auto modConst = std::make_shared<awst::IntegerConstant>();
				modConst->sourceLocation = loc;
				modConst->wtype = awst::WType::uint64Type();
				modConst->value = std::to_string(modVal);

				auto masked = std::make_shared<awst::UInt64BinaryOperation>();
				masked->sourceLocation = loc;
				masked->wtype = awst::WType::uint64Type();
				masked->left = std::move(result);
				masked->op = awst::UInt64BinaryOperator::Mod;
				masked->right = std::move(modConst);
				result = std::move(masked);
			}
		}
	}

	// Checked arithmetic: for non-unchecked add/sub/mul on integer types,
	// assert the result fits in the Solidity type's bit width.
	// AVM biguint is arbitrary-precision and uint64 is 64-bit, so neither
	// naturally detects overflow for narrower Solidity types (uint8, uint16, etc.).
	if (!m_inUncheckedBlock
		&& (result->wtype == awst::WType::biguintType()
			|| result->wtype == awst::WType::uint64Type()))
	{
		auto const* solType = dynamic_cast<solidity::frontend::IntegerType const*>(
			_node.annotation().type);
		if (solType)
		{
			unsigned bits = solType->numBits();
			bool isSigned = solType->isSigned();
			auto checkOp = _node.getOperator();
			bool needsCheck = (checkOp == Token::Add || checkOp == Token::AssignAdd
				|| checkOp == Token::Sub || checkOp == Token::AssignSub
				|| checkOp == Token::Mul || checkOp == Token::AssignMul
				|| checkOp == Token::Exp);

			// For biguint: check bits < 256 (256-bit is full range, no overflow possible)
			// For uint64: check bits < 64 (e.g. uint8, uint16, uint32 need checking)
			unsigned maxBits = (result->wtype == awst::WType::biguintType()) ? 256 : 64;
			if (needsCheck && !isSigned && bits < maxBits)
			{
				// Emit: __checked_result = <result>
				// assert(__checked_result <= typeMax, "overflow")
				// <use __checked_result>
				static int checkedCounter = 0;
				std::string tmpName = "__checked_" + std::to_string(checkedCounter++);
				auto* resType = result->wtype;

				auto tmpVar = std::make_shared<awst::VarExpression>();
				tmpVar->sourceLocation = loc;
				tmpVar->wtype = resType;
				tmpVar->name = tmpName;

				auto assign = std::make_shared<awst::AssignmentStatement>();
				assign->sourceLocation = loc;
				assign->target = tmpVar;
				assign->value = std::move(result);
				m_prePendingStatements.push_back(std::move(assign));

				auto maxConst = std::make_shared<awst::IntegerConstant>();
				maxConst->sourceLocation = loc;
				maxConst->wtype = resType;
				if (resType == awst::WType::biguintType())
				{
					solidity::u256 maxVal = (solidity::u256(1) << bits) - 1;
					maxConst->value = maxVal.str();
				}
				else
				{
					uint64_t maxVal = (uint64_t(1) << bits) - 1;
					maxConst->value = std::to_string(maxVal);
				}

				auto cmp = std::make_shared<awst::NumericComparisonExpression>();
				cmp->sourceLocation = loc;
				cmp->wtype = awst::WType::boolType();
				cmp->lhs = tmpVar;
				cmp->op = awst::NumericComparison::Lte;
				cmp->rhs = std::move(maxConst);

				auto assertStmt = std::make_shared<awst::ExpressionStatement>();
				assertStmt->sourceLocation = loc;
				auto assertExpr = std::make_shared<awst::AssertExpression>();
				assertExpr->sourceLocation = loc;
				assertExpr->wtype = awst::WType::voidType();
				assertExpr->condition = std::move(cmp);
				assertExpr->errorMessage = "overflow";
				assertStmt->expr = std::move(assertExpr);
				m_prePendingStatements.push_back(std::move(assertStmt));

				result = tmpVar;
			}
		}
	}

	push(std::move(result));
	return false;
}

} // namespace puyasol::builder
