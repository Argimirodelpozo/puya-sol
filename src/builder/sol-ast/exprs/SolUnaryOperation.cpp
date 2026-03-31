/// @file SolUnaryOperation.cpp
/// Migrated from UnaryOperationBuilder.cpp.

#include "builder/sol-ast/exprs/SolUnaryOperation.h"
#include "builder/sol-eb/NodeBuilder.h"
#include "builder/sol-eb/BuilderOps.h"
#include "builder/storage/StorageMapper.h"
#include "builder/sol-types/TypeMapper.h"

#include <libsolidity/ast/AST.h>

namespace puyasol::builder::sol_ast
{

using namespace solidity::frontend;
using Token = solidity::frontend::Token;

SolUnaryOperation::SolUnaryOperation(
	eb::BuilderContext& _ctx, UnaryOperation const& _node)
	: SolExpression(_ctx, _node), m_unaryOp(_node)
{
}

bool SolUnaryOperation::isBigUInt(awst::WType const* _type) const
{
	return _type == awst::WType::biguintType();
}

std::shared_ptr<awst::Expression> SolUnaryOperation::handleNot(
	std::shared_ptr<awst::Expression> _operand)
{
	auto e = std::make_shared<awst::Not>();
	e->sourceLocation = m_loc;
	e->wtype = awst::WType::boolType();
	e->expr = std::move(_operand);
	return e;
}

std::shared_ptr<awst::Expression> SolUnaryOperation::handleNegate(
	std::shared_ptr<awst::Expression> _operand)
{
	auto zero = std::make_shared<awst::IntegerConstant>();
	zero->sourceLocation = m_loc;
	zero->wtype = _operand->wtype;
	zero->value = "0";
	if (isBigUInt(_operand->wtype))
	{
		auto e = std::make_shared<awst::BigUIntBinaryOperation>();
		e->sourceLocation = m_loc;
		e->wtype = awst::WType::biguintType();
		e->left = std::move(zero);
		e->op = awst::BigUIntBinaryOperator::Sub;
		e->right = std::move(_operand);
		return e;
	}
	auto e = std::make_shared<awst::UInt64BinaryOperation>();
	e->sourceLocation = m_loc;
	e->wtype = awst::WType::uint64Type();
	e->left = std::move(zero);
	e->op = awst::UInt64BinaryOperator::Sub;
	e->right = std::move(_operand);
	return e;
}

std::shared_ptr<awst::Expression> SolUnaryOperation::handleBitNot(
	std::shared_ptr<awst::Expression> _operand)
{
	auto* resultType = _operand->wtype;
	if (_operand->wtype == awst::WType::uint64Type())
	{
		auto maxVal = std::make_shared<awst::IntegerConstant>();
		maxVal->sourceLocation = m_loc;
		maxVal->wtype = awst::WType::uint64Type();
		maxVal->value = "18446744073709551615";
		auto xorOp = std::make_shared<awst::UInt64BinaryOperation>();
		xorOp->sourceLocation = m_loc;
		xorOp->wtype = awst::WType::uint64Type();
		xorOp->left = std::move(_operand);
		xorOp->right = std::move(maxVal);
		xorOp->op = awst::UInt64BinaryOperator::BitXor;
		return xorOp;
	}
	auto expr = std::move(_operand);
	if (expr->wtype == awst::WType::biguintType())
	{
		auto cast = std::make_shared<awst::ReinterpretCast>();
		cast->sourceLocation = m_loc;
		cast->wtype = awst::WType::bytesType();
		cast->expr = std::move(expr);
		expr = std::move(cast);
	}
	auto e = std::make_shared<awst::BytesUnaryOperation>();
	e->sourceLocation = m_loc;
	e->wtype = expr->wtype;
	e->op = awst::BytesUnaryOperator::BitInvert;
	e->expr = std::move(expr);
	if (resultType == awst::WType::biguintType())
	{
		auto castBack = std::make_shared<awst::ReinterpretCast>();
		castBack->sourceLocation = m_loc;
		castBack->wtype = resultType;
		castBack->expr = std::move(e);
		return castBack;
	}
	return e;
}

std::shared_ptr<awst::Expression> SolUnaryOperation::handleIncDec(
	std::shared_ptr<awst::Expression> _operand)
{
	bool isPrefix = m_unaryOp.isPrefixOperation();
	bool isInc = (m_unaryOp.getOperator() == Token::Inc);

	bool isSigned = false;
	if (auto const* intType = dynamic_cast<IntegerType const*>(
			m_unaryOp.subExpression().annotation().type))
		isSigned = intType->isSigned();

	// Unwrap BoxValueExpression for reads
	if (dynamic_cast<awst::BoxValueExpression const*>(_operand.get()))
	{
		auto defaultVal = builder::StorageMapper::makeDefaultValue(_operand->wtype, m_loc);
		auto stateGet = std::make_shared<awst::StateGet>();
		stateGet->sourceLocation = m_loc;
		stateGet->wtype = _operand->wtype;
		stateGet->field = _operand;
		stateGet->defaultValue = defaultVal;
		_operand = std::move(stateGet);
	}

	static const std::string pow256 =
		"115792089237316195423570985008687907853269984665640564039457584007913129639936";
	static const std::string pow256Minus1 =
		"115792089237316195423570985008687907853269984665640564039457584007913129639935";

	auto makeNewValue = [&](std::shared_ptr<awst::Expression> base)
		-> std::shared_ptr<awst::Expression>
	{
		auto one = std::make_shared<awst::IntegerConstant>();
		one->sourceLocation = m_loc;
		one->wtype = base->wtype;
		one->value = "1";

		if (isBigUInt(base->wtype) && isSigned)
		{
			std::shared_ptr<awst::Expression> added;
			if (isInc)
			{
				auto add = std::make_shared<awst::BigUIntBinaryOperation>();
				add->sourceLocation = m_loc;
				add->wtype = awst::WType::biguintType();
				add->left = std::move(base);
				add->op = awst::BigUIntBinaryOperator::Add;
				add->right = std::move(one);
				added = std::move(add);
			}
			else
			{
				auto offset = std::make_shared<awst::IntegerConstant>();
				offset->sourceLocation = m_loc;
				offset->wtype = awst::WType::biguintType();
				offset->value = pow256Minus1;
				auto add = std::make_shared<awst::BigUIntBinaryOperation>();
				add->sourceLocation = m_loc;
				add->wtype = awst::WType::biguintType();
				add->left = std::move(base);
				add->op = awst::BigUIntBinaryOperator::Add;
				add->right = std::move(offset);
				added = std::move(add);
			}
			auto modConst = std::make_shared<awst::IntegerConstant>();
			modConst->sourceLocation = m_loc;
			modConst->wtype = awst::WType::biguintType();
			modConst->value = pow256;
			auto mod = std::make_shared<awst::BigUIntBinaryOperation>();
			mod->sourceLocation = m_loc;
			mod->wtype = awst::WType::biguintType();
			mod->left = std::move(added);
			mod->op = awst::BigUIntBinaryOperator::Mod;
			mod->right = std::move(modConst);
			return mod;
		}
		else if (isBigUInt(base->wtype))
		{
			auto bin = std::make_shared<awst::BigUIntBinaryOperation>();
			bin->sourceLocation = m_loc;
			bin->wtype = awst::WType::biguintType();
			bin->left = std::move(base);
			bin->op = isInc ? awst::BigUIntBinaryOperator::Add : awst::BigUIntBinaryOperator::Sub;
			bin->right = std::move(one);
			return bin;
		}
		else
		{
			auto bin = std::make_shared<awst::UInt64BinaryOperation>();
			bin->sourceLocation = m_loc;
			bin->wtype = awst::WType::uint64Type();
			bin->left = std::move(base);
			bin->op = isInc ? awst::UInt64BinaryOperator::Add : awst::UInt64BinaryOperator::Sub;
			bin->right = std::move(one);
			return bin;
		}
	};

	if (isPrefix)
	{
		auto newValue = makeNewValue(_operand);
		auto assignExpr = std::make_shared<awst::AssignmentExpression>();
		assignExpr->sourceLocation = m_loc;
		assignExpr->wtype = _operand->wtype;
		assignExpr->target = buildExpr(m_unaryOp.subExpression());
		assignExpr->value = std::move(newValue);
		return assignExpr;
	}
	else
	{
		auto singleEval = std::make_shared<awst::SingleEvaluation>();
		singleEval->sourceLocation = m_loc;
		singleEval->wtype = _operand->wtype;
		singleEval->source = _operand;
		singleEval->id = static_cast<int>(m_unaryOp.id());

		auto newValue = makeNewValue(singleEval);

		auto incrStmt = std::make_shared<awst::AssignmentStatement>();
		incrStmt->sourceLocation = m_loc;
		incrStmt->target = buildExpr(m_unaryOp.subExpression());
		incrStmt->value = std::move(newValue);
		m_ctx.pendingStatements.push_back(std::move(incrStmt));

		return singleEval;
	}
}

std::shared_ptr<awst::Expression> SolUnaryOperation::handleDelete(
	std::shared_ptr<awst::Expression> _operand)
{
	auto target = buildExpr(m_unaryOp.subExpression());

	if (dynamic_cast<awst::BoxValueExpression const*>(target.get()))
	{
		auto stateDelete = std::make_shared<awst::StateDelete>();
		stateDelete->sourceLocation = m_loc;
		stateDelete->wtype = awst::WType::boolType();
		stateDelete->field = target;
		auto stmt = std::make_shared<awst::ExpressionStatement>();
		stmt->sourceLocation = m_loc;
		stmt->expr = std::move(stateDelete);
		m_ctx.pendingStatements.push_back(std::move(stmt));
		return _operand;
	}

	// Unwrap ARC4Decode
	if (auto const* decodeExpr = dynamic_cast<awst::ARC4Decode const*>(target.get()))
		target = decodeExpr->value;

	// ARC4Struct field deletion — copy-on-write with zeroed field
	if (auto const* fieldExpr = dynamic_cast<awst::FieldExpression const*>(target.get()))
	{
		auto const* arc4StructType = dynamic_cast<awst::ARC4Struct const*>(fieldExpr->base->wtype);
		if (arc4StructType)
		{
			auto base = fieldExpr->base;
			std::string fieldName = fieldExpr->name;

			auto readBase = base;
			if (dynamic_cast<awst::BoxValueExpression const*>(base.get()))
			{
				auto stateGet = std::make_shared<awst::StateGet>();
				stateGet->sourceLocation = m_loc;
				stateGet->wtype = base->wtype;
				stateGet->field = base;
				stateGet->defaultValue = builder::StorageMapper::makeDefaultValue(base->wtype, m_loc);
				readBase = stateGet;
			}

			awst::WType const* arc4FieldType = nullptr;
			for (auto const& [fname, ftype]: arc4StructType->fields())
				if (fname == fieldName) { arc4FieldType = ftype; break; }

			auto zeroVal = builder::StorageMapper::makeDefaultValue(
				arc4FieldType ? arc4FieldType : fieldExpr->wtype, m_loc);

			auto newStruct = std::make_shared<awst::NewStruct>();
			newStruct->sourceLocation = m_loc;
			newStruct->wtype = arc4StructType;
			for (auto const& [fname, ftype]: arc4StructType->fields())
			{
				if (fname == fieldName)
					newStruct->values[fname] = std::move(zeroVal);
				else
				{
					auto field = std::make_shared<awst::FieldExpression>();
					field->sourceLocation = m_loc;
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
			assignStmt->sourceLocation = m_loc;
			assignStmt->target = std::move(writeTarget);
			assignStmt->value = std::move(newStruct);
			m_ctx.pendingStatements.push_back(std::move(assignStmt));
			return _operand;
		}
	}

	// Default: assign zero value
	auto defaultVal = builder::StorageMapper::makeDefaultValue(target->wtype, m_loc);
	if (auto const* sg = dynamic_cast<awst::StateGet const*>(target.get()))
		target = sg->field;

	auto assignStmt = std::make_shared<awst::AssignmentStatement>();
	assignStmt->sourceLocation = m_loc;
	assignStmt->target = target;
	assignStmt->value = std::move(defaultVal);
	m_ctx.pendingStatements.push_back(std::move(assignStmt));
	return _operand;
}

std::shared_ptr<awst::Expression> SolUnaryOperation::toAwst()
{
	auto operand = buildExpr(m_unaryOp.subExpression());

	// Try sol-eb builder dispatch for Not/Sub/BitNot
	{
		eb::BuilderUnaryOp builderOp;
		bool hasUnaryOp = true;
		switch (m_unaryOp.getOperator())
		{
		case Token::Not: builderOp = eb::BuilderUnaryOp::LogicalNot; break;
		case Token::Sub: builderOp = eb::BuilderUnaryOp::Negative; break;
		case Token::BitNot: builderOp = eb::BuilderUnaryOp::BitInvert; break;
		default: hasUnaryOp = false; break;
		}
		if (hasUnaryOp)
		{
			auto* solType = m_unaryOp.subExpression().annotation().type;
			auto builder = m_ctx.builderForInstance(solType, operand);
			if (builder)
			{
				auto result = builder->unary_op(builderOp, m_loc);
				if (result)
					return result->resolve();
			}
		}
	}

	switch (m_unaryOp.getOperator())
	{
	case Token::Not:    return handleNot(std::move(operand));
	case Token::Sub:    return handleNegate(std::move(operand));
	case Token::BitNot: return handleBitNot(std::move(operand));
	case Token::Inc:
	case Token::Dec:    return handleIncDec(std::move(operand));
	case Token::Delete: return handleDelete(std::move(operand));
	default:            return operand;
	}
}

} // namespace puyasol::builder::sol_ast
