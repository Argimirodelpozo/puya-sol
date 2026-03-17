/// @file UnaryOperationBuilder.cpp
/// Handles unary operations (!, ~, ++, --, type casts).

#include "builder/expressions/ExpressionBuilder.h"
#include "builder/storage/StorageMapper.h"
#include "Logger.h"

#include <libsolidity/ast/TypeProvider.h>

#include <algorithm>
#include <sstream>

namespace puyasol::builder
{

bool ExpressionBuilder::visit(solidity::frontend::UnaryOperation const& _node)
{
	auto loc = makeLoc(_node.location());
	auto operand = build(_node.subExpression());

	using Token = solidity::frontend::Token;

	switch (_node.getOperator())
	{
	case Token::Not:
	{
		auto e = std::make_shared<awst::Not>();
		e->sourceLocation = loc;
		e->wtype = awst::WType::boolType();
		e->expr = std::move(operand);
		push(e);
		break;
	}
	case Token::Sub:
	{
		// Unary minus: negate operand
		// For constant operands, fold to two's complement directly to avoid AVM underflow
		auto const* intConst = dynamic_cast<awst::IntegerConstant const*>(operand.get());
		if (intConst && !intConst->value.empty() && intConst->value != "0")
		{
			try
			{
				unsigned long long val = std::stoull(intConst->value);
				if (val > 0)
				{
					if (isBigUInt(operand->wtype))
					{
						// biguint: two's complement mod 2^256
						// Use u256 from libsolutil
						solidity::u256 mod256 = solidity::u256(1) << 256;
						solidity::u256 result = mod256 - solidity::u256(val);
						auto e = std::make_shared<awst::IntegerConstant>();
						e->sourceLocation = loc;
						e->wtype = awst::WType::biguintType();
						e->value = result.str();
						push(e);
					}
					else
					{
						// uint64: two's complement mod 2^64
						// UINT64_MAX + 1 - val, but since UINT64_MAX+1 overflows,
						// use: (UINT64_MAX - val) + 1
						unsigned long long result = (UINT64_MAX - val) + 1ULL;
						auto e = std::make_shared<awst::IntegerConstant>();
						e->sourceLocation = loc;
						e->wtype = awst::WType::uint64Type();
						e->value = std::to_string(result);
						push(e);
					}
					break;
				}
			}
			catch (...) {} // fall through to runtime subtraction
		}
		// Non-constant: 0 - operand (may underflow for uint64 — signed types
		// should avoid this path by using constant expressions)
		auto* zeroWtype = operand->wtype;
		if (zeroWtype != awst::WType::uint64Type() && zeroWtype != awst::WType::biguintType())
			zeroWtype = awst::WType::uint64Type();
		auto zero = std::make_shared<awst::IntegerConstant>();
		zero->sourceLocation = loc;
		zero->wtype = zeroWtype;
		zero->value = "0";

		if (isBigUInt(operand->wtype))
		{
			// Negate biguint using two's complement: ~operand + 1
			// This avoids (2^256 - operand) which the puya optimizer folds into 0 - operand.
			// ~operand is bitwise NOT on the bytes (b~), then add 1.
			// For operand = 0: ~0 + 1 = 2^256, but this is only used in contexts
			// where operand != 0 (e.g., -b in `a - uint256(-b)` where b is negative int256).
			auto castToBytes = std::make_shared<awst::ReinterpretCast>();
			castToBytes->sourceLocation = loc;
			castToBytes->wtype = awst::WType::bytesType();
			castToBytes->expr = std::move(operand);

			auto bitInvert = std::make_shared<awst::BytesUnaryOperation>();
			bitInvert->sourceLocation = loc;
			bitInvert->wtype = awst::WType::bytesType();
			bitInvert->op = awst::BytesUnaryOperator::BitInvert;
			bitInvert->expr = std::move(castToBytes);

			auto castBack = std::make_shared<awst::ReinterpretCast>();
			castBack->sourceLocation = loc;
			castBack->wtype = awst::WType::biguintType();
			castBack->expr = std::move(bitInvert);

			auto one = std::make_shared<awst::IntegerConstant>();
			one->sourceLocation = loc;
			one->wtype = awst::WType::biguintType();
			one->value = "1";

			auto addOne = std::make_shared<awst::BigUIntBinaryOperation>();
			addOne->sourceLocation = loc;
			addOne->wtype = awst::WType::biguintType();
			addOne->left = std::move(castBack);
			addOne->op = awst::BigUIntBinaryOperator::Add;
			addOne->right = std::move(one);
			push(addOne);
		}
		else
		{
			auto e = std::make_shared<awst::UInt64BinaryOperation>();
			e->sourceLocation = loc;
			e->wtype = awst::WType::uint64Type();
			e->left = std::move(zero);
			e->op = awst::UInt64BinaryOperator::Sub;
			e->right = std::move(operand);
			push(e);
		}
		break;
	}
	case Token::Inc:
	case Token::Dec:
	{
		// i++ / i-- / ++i / --i → assignment expression: i = i +/- 1
		// Prefix (++i): result is the new value
		// Postfix (i++): result is the old value (before increment)
		bool isPrefix = _node.isPrefixOperation();
		auto isInc = (_node.getOperator() == Token::Inc);

		// For box-stored mappings, the operand is a bare BoxValueExpression
		// (willBeWrittenTo=true skips StateGet wrapping). Wrap it in StateGet
		// with a default value so the read works for uninitialized boxes.
		if (dynamic_cast<awst::BoxValueExpression const*>(operand.get()))
		{
			auto defaultVal = StorageMapper::makeDefaultValue(operand->wtype, loc);
			auto stateGet = std::make_shared<awst::StateGet>();
			stateGet->sourceLocation = loc;
			stateGet->wtype = operand->wtype;
			stateGet->field = operand;
			stateGet->defaultValue = defaultVal;
			operand = std::move(stateGet);
		}

		if (isPrefix)
		{
			// ++i / --i: compute new value, assign, return new value
			auto one = std::make_shared<awst::IntegerConstant>();
			one->sourceLocation = loc;
			one->wtype = operand->wtype;
			one->value = "1";

			std::shared_ptr<awst::Expression> newValue;
			if (isBigUInt(operand->wtype))
			{
				auto binOp = std::make_shared<awst::BigUIntBinaryOperation>();
				binOp->sourceLocation = loc;
				binOp->wtype = awst::WType::biguintType();
				binOp->left = operand;
				binOp->op = isInc ? awst::BigUIntBinaryOperator::Add : awst::BigUIntBinaryOperator::Sub;
				binOp->right = std::move(one);
				newValue = std::move(binOp);
			}
			else
			{
				auto binOp = std::make_shared<awst::UInt64BinaryOperation>();
				binOp->sourceLocation = loc;
				binOp->wtype = awst::WType::uint64Type();
				binOp->left = operand;
				binOp->op = isInc ? awst::UInt64BinaryOperator::Add : awst::UInt64BinaryOperator::Sub;
				binOp->right = std::move(one);
				newValue = std::move(binOp);
			}

			auto assignExpr = std::make_shared<awst::AssignmentExpression>();
			assignExpr->sourceLocation = loc;
			assignExpr->wtype = operand->wtype;
			auto target = build(_node.subExpression());
			assignExpr->target = std::move(target);
			assignExpr->value = std::move(newValue);
			push(assignExpr);
		}
		else
		{
			// i++ / i--: capture old value via SingleEvaluation, increment as side effect
			// SingleEvaluation ensures the operand is read exactly once and cached.
			// Both the return value and the increment expression share the same
			// SingleEvaluation, so whichever evaluates first captures the old value.
			auto singleEval = std::make_shared<awst::SingleEvaluation>();
			singleEval->sourceLocation = loc;
			singleEval->wtype = operand->wtype;
			singleEval->source = operand;
			singleEval->id = static_cast<int>(_node.id());

			auto one = std::make_shared<awst::IntegerConstant>();
			one->sourceLocation = loc;
			one->wtype = operand->wtype;
			one->value = "1";

			// Build new value: singleEval +/- 1
			std::shared_ptr<awst::Expression> newValue;
			if (isBigUInt(operand->wtype))
			{
				auto binOp = std::make_shared<awst::BigUIntBinaryOperation>();
				binOp->sourceLocation = loc;
				binOp->wtype = awst::WType::biguintType();
				binOp->left = singleEval;
				binOp->op = isInc ? awst::BigUIntBinaryOperator::Add : awst::BigUIntBinaryOperator::Sub;
				binOp->right = std::move(one);
				newValue = std::move(binOp);
			}
			else
			{
				auto binOp = std::make_shared<awst::UInt64BinaryOperation>();
				binOp->sourceLocation = loc;
				binOp->wtype = awst::WType::uint64Type();
				binOp->left = singleEval;
				binOp->op = isInc ? awst::UInt64BinaryOperator::Add : awst::UInt64BinaryOperator::Sub;
				binOp->right = std::move(one);
				newValue = std::move(binOp);
			}

			// Emit increment as a pending statement
			auto incrStmt = std::make_shared<awst::AssignmentStatement>();
			incrStmt->sourceLocation = loc;
			auto target = build(_node.subExpression());
			incrStmt->target = std::move(target);
			incrStmt->value = std::move(newValue);
			m_pendingStatements.push_back(std::move(incrStmt));

			// Return the SingleEvaluation (old value) as the expression result
			push(singleEval);
		}
		break;
	}
	case Token::BitNot:
	{
		// Bitwise NOT: ~operand
		auto expr = std::move(operand);
		auto* resultType = expr->wtype;

		if (expr->wtype == awst::WType::uint64Type())
		{
			// For uint64: ~x = x ^ 0xFFFFFFFFFFFFFFFF
			auto maxVal = std::make_shared<awst::IntegerConstant>();
			maxVal->sourceLocation = loc;
			maxVal->wtype = awst::WType::uint64Type();
			maxVal->value = "18446744073709551615"; // 2^64 - 1
			auto xorOp = std::make_shared<awst::UInt64BinaryOperation>();
			xorOp->sourceLocation = loc;
			xorOp->wtype = awst::WType::uint64Type();
			xorOp->left = std::move(expr);
			xorOp->right = std::move(maxVal);
			xorOp->op = awst::UInt64BinaryOperator::BitXor;
			push(std::move(xorOp));
		}
		else
		{
			// For biguint/bytes: use b~ opcode
			if (expr->wtype == awst::WType::biguintType())
			{
				auto cast = std::make_shared<awst::ReinterpretCast>();
				cast->sourceLocation = loc;
				cast->wtype = awst::WType::bytesType();
				cast->expr = std::move(expr);
				expr = std::move(cast);
			}
			auto e = std::make_shared<awst::BytesUnaryOperation>();
			e->sourceLocation = loc;
			e->wtype = expr->wtype;
			e->op = awst::BytesUnaryOperator::BitInvert;
			e->expr = std::move(expr);
			if (resultType == awst::WType::biguintType())
			{
				auto castBack = std::make_shared<awst::ReinterpretCast>();
				castBack->sourceLocation = loc;
				castBack->wtype = resultType;
				castBack->expr = std::move(e);
				push(std::move(castBack));
			}
			else
				push(e);
		}
		break;
	}
	case Token::Delete:
	{
		// delete x → StateDelete for mapping values (box delete),
		//            or assign zero/default for state vars and locals
		auto target = build(_node.subExpression());

		// If target is a BoxValueExpression, emit StateDelete using the box as the field
		if (dynamic_cast<awst::BoxValueExpression const*>(target.get()))
		{
			auto stateDelete = std::make_shared<awst::StateDelete>();
			stateDelete->sourceLocation = loc;
			stateDelete->wtype = awst::WType::boolType();
			stateDelete->field = target; // The BoxValueExpression IS the field
			m_pendingStatements.push_back(std::make_shared<awst::ExpressionStatement>());
			m_pendingStatements.back()->sourceLocation = loc;
			static_cast<awst::ExpressionStatement*>(m_pendingStatements.back().get())->expr = std::move(stateDelete);
		}
		else
		{
			// Unwrap ARC4Decode — not a valid Lvalue
			if (auto const* decodeExpr = dynamic_cast<awst::ARC4Decode const*>(target.get()))
				target = decodeExpr->value;

			// Check if target is an ARC4Struct field — needs copy-on-write
			if (auto const* fieldExpr = dynamic_cast<awst::FieldExpression const*>(target.get()))
			{
				auto const* arc4StructType = dynamic_cast<awst::ARC4Struct const*>(fieldExpr->base->wtype);
				if (arc4StructType)
				{
					auto base = fieldExpr->base;
					std::string fieldName = fieldExpr->name;

					// Ensure base is readable
					auto readBase = base;
					if (dynamic_cast<awst::BoxValueExpression const*>(base.get()))
					{
						auto stateGet = std::make_shared<awst::StateGet>();
						stateGet->sourceLocation = loc;
						stateGet->wtype = base->wtype;
						stateGet->field = base;
						stateGet->defaultValue = StorageMapper::makeDefaultValue(base->wtype, loc);
						readBase = stateGet;
					}

					// Build zero value for the deleted field (ARC4-encoded)
					awst::WType const* arc4FieldType = nullptr;
					for (auto const& [fname, ftype]: arc4StructType->fields())
						if (fname == fieldName) { arc4FieldType = ftype; break; }

					auto zeroVal = StorageMapper::makeDefaultValue(
						arc4FieldType ? arc4FieldType : fieldExpr->wtype, loc);

					// Build NewStruct with all fields copied, deleted one zeroed
					auto newStruct = std::make_shared<awst::NewStruct>();
					newStruct->sourceLocation = loc;
					newStruct->wtype = arc4StructType;
					for (auto const& [fname, ftype]: arc4StructType->fields())
					{
						if (fname == fieldName)
							newStruct->values[fname] = std::move(zeroVal);
						else
						{
							auto field = std::make_shared<awst::FieldExpression>();
							field->sourceLocation = loc;
							field->base = readBase;
							field->name = fname;
							field->wtype = ftype;
							newStruct->values[fname] = std::move(field);
						}
					}

					auto writeTarget = base;
					if (auto const* sg = dynamic_cast<awst::StateGet const*>(base.get()))
						writeTarget = sg->field;

					auto assignStmt = std::make_shared<awst::AssignmentStatement>();
					assignStmt->sourceLocation = loc;
					assignStmt->target = std::move(writeTarget);
					assignStmt->value = std::move(newStruct);
					m_pendingStatements.push_back(std::move(assignStmt));

					// Push dummy and break
					push(std::move(operand));
					break;
				}
			}

			// For other targets: assign the zero/default value
			std::shared_ptr<awst::Expression> defaultVal;
			if (target->wtype == awst::WType::accountType())
			{
				// address → zero address constant
				auto addr = std::make_shared<awst::AddressConstant>();
				addr->sourceLocation = loc;
				addr->wtype = awst::WType::accountType();
				addr->value = "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAY5HFKQ";
				defaultVal = std::move(addr);
			}
			else if (isBigUInt(target->wtype))
			{
				auto intVal = std::make_shared<awst::IntegerConstant>();
				intVal->sourceLocation = loc;
				intVal->wtype = awst::WType::biguintType();
				intVal->value = "0";
				defaultVal = std::move(intVal);
			}
			else if (target->wtype && target->wtype->kind() == awst::WTypeKind::Bytes)
			{
				// bytes/bytes[N]/string → empty bytes
				auto bytesVal = std::make_shared<awst::BytesConstant>();
				bytesVal->sourceLocation = loc;
				bytesVal->wtype = target->wtype;
				bytesVal->encoding = awst::BytesEncoding::Base16;
				bytesVal->value = {};
				defaultVal = std::move(bytesVal);
			}
			else
			{
				auto intVal = std::make_shared<awst::IntegerConstant>();
				intVal->sourceLocation = loc;
				intVal->wtype = awst::WType::uint64Type();
				intVal->value = "0";
				defaultVal = std::move(intVal);
			}

			// Unwrap StateGet for write targets
			if (auto const* sg = dynamic_cast<awst::StateGet const*>(target.get()))
				target = sg->field;

			auto assignStmt = std::make_shared<awst::AssignmentStatement>();
			assignStmt->sourceLocation = loc;
			assignStmt->target = target;
			assignStmt->value = std::move(defaultVal);
			m_pendingStatements.push_back(std::move(assignStmt));
		}

		// Delete expression evaluates to void; push a dummy
		push(std::move(operand));
		break;
	}
	default:
	{
		// Fallback: just pass through the operand
		push(std::move(operand));
		break;
	}
	}

	return false;
}


} // namespace puyasol::builder
