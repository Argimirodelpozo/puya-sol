/// @file SolAssignment.cpp
/// Migrated from AssignmentBuilder.cpp.

#include "builder/sol-ast/exprs/SolAssignment.h"
#include "builder/sol-eb/AssignmentHelper.h"
#include "builder/storage/StorageMapper.h"
#include "builder/sol-types/TypeMapper.h"
#include "builder/sol-types/TypeCoercion.h"
#include "Logger.h"

#include <libsolidity/ast/AST.h>

namespace puyasol::builder::sol_ast
{

using namespace solidity::frontend;
using Token = solidity::frontend::Token;

SolAssignment::SolAssignment(eb::BuilderContext& _ctx, Assignment const& _node)
	: SolExpression(_ctx, _node), m_assignment(_node)
{
}

std::shared_ptr<awst::Expression> SolAssignment::buildTupleWithUpdatedField(
	std::shared_ptr<awst::Expression> _base,
	std::string const& _fieldName,
	std::shared_ptr<awst::Expression> _newValue)
{
	auto const* tupleType = dynamic_cast<awst::WTuple const*>(_base->wtype);
	auto const& names = *tupleType->names();
	auto const& types = tupleType->types();

	auto tuple = std::make_shared<awst::TupleExpression>();
	tuple->sourceLocation = m_loc;
	tuple->wtype = _base->wtype;

	for (size_t i = 0; i < names.size(); ++i)
	{
		if (names[i] == _fieldName)
			tuple->items.push_back(std::move(_newValue));
		else
		{
			auto field = std::make_shared<awst::FieldExpression>();
			field->sourceLocation = m_loc;
			field->base = _base;
			field->name = names[i];
			field->wtype = types[i];
			tuple->items.push_back(std::move(field));
		}
	}
	return tuple;
}

std::shared_ptr<awst::Expression> SolAssignment::handleTupleAssignment(
	std::shared_ptr<awst::Expression> _target,
	std::shared_ptr<awst::Expression> _value)
{
	auto const* tupleTarget = dynamic_cast<awst::TupleExpression const*>(_target.get());
	auto const& items = tupleTarget->items;

	for (size_t i = 0; i < items.size(); ++i)
	{
		auto item = items[i];
		auto itemExpr = std::make_shared<awst::TupleItemExpression>();
		itemExpr->sourceLocation = m_loc;
		itemExpr->wtype = item->wtype;
		itemExpr->base = _value;
		itemExpr->index = static_cast<int>(i);

		auto assignTarget = item;
		if (auto const* decodeExpr = dynamic_cast<awst::ARC4Decode const*>(item.get()))
			assignTarget = decodeExpr->value;
		if (auto const* sg = dynamic_cast<awst::StateGet const*>(assignTarget.get()))
			assignTarget = sg->field;

		std::shared_ptr<awst::Expression> assignValue = std::move(itemExpr);
		if (assignTarget->wtype != assignValue->wtype)
		{
			bool targetIsArc4 = false;
			switch (assignTarget->wtype->kind())
			{
			case awst::WTypeKind::ARC4UIntN:
			case awst::WTypeKind::ARC4StaticArray:
			case awst::WTypeKind::ARC4DynamicArray:
			case awst::WTypeKind::ARC4Struct:
				targetIsArc4 = true; break;
			default: break;
			}
			if (targetIsArc4)
			{
				assignValue = builder::TypeCoercion::stringToBytes(std::move(assignValue), m_loc);
				auto encode = std::make_shared<awst::ARC4Encode>();
				encode->sourceLocation = m_loc;
				encode->wtype = assignTarget->wtype;
				encode->value = std::move(assignValue);
				assignValue = std::move(encode);
			}
			else
				assignValue = builder::TypeCoercion::implicitNumericCast(
					std::move(assignValue), assignTarget->wtype, m_loc);
		}

		// ARC4Struct field — copy-on-write (simplified: delegate to struct field handler below)
		if (auto const* fieldExpr = dynamic_cast<awst::FieldExpression const*>(assignTarget.get()))
		{
			auto const* structType = dynamic_cast<awst::ARC4Struct const*>(fieldExpr->base->wtype);
			if (!structType)
				if (auto const* sg = dynamic_cast<awst::StateGet const*>(fieldExpr->base.get()))
					structType = dynamic_cast<awst::ARC4Struct const*>(sg->field->wtype);

			if (structType)
			{
				auto result = handleStructFieldAssignment(fieldExpr, std::move(assignValue), assignTarget);
				if (result) continue;
			}
		}

		if (assignTarget->wtype != assignValue->wtype
			&& assignTarget->wtype->kind() == awst::WTypeKind::ARC4StaticArray)
		{
			auto enc = std::make_shared<awst::ARC4Encode>();
			enc->sourceLocation = m_loc;
			enc->wtype = assignTarget->wtype;
			enc->value = std::move(assignValue);
			assignValue = std::move(enc);
		}

		auto e = std::make_shared<awst::AssignmentExpression>();
		e->sourceLocation = m_loc;
		e->wtype = assignTarget->wtype;
		e->target = std::move(assignTarget);
		e->value = std::move(assignValue);
		auto stmt = std::make_shared<awst::ExpressionStatement>();
		stmt->sourceLocation = m_loc;
		stmt->expr = e;
		m_ctx.pendingStatements.push_back(std::move(stmt));
	}
	return _value;
}

std::shared_ptr<awst::Expression> SolAssignment::handleBytesElementAssignment(
	awst::IndexExpression const* _indexExpr,
	std::shared_ptr<awst::Expression> _value)
{
	Token op = m_assignment.assignmentOperator();

	if (op != Token::Assign)
	{
		auto currentValue = buildExpr(m_assignment.leftHandSide());
		auto* solType = m_assignment.leftHandSide().annotation().type;
		auto builderResult = eb::AssignmentHelper::tryComputeCompoundValue(
			m_ctx, op, solType, currentValue, _value, m_loc);
		if (builderResult)
			_value = std::move(builderResult);
		else
			_value = m_ctx.buildBinaryOp(op, std::move(currentValue), std::move(_value),
				_indexExpr->wtype, m_loc);
	}

	// Coerce value to single byte
	if (_value->wtype == awst::WType::uint64Type())
	{
		auto itob = std::make_shared<awst::IntrinsicCall>();
		itob->sourceLocation = m_loc;
		itob->wtype = awst::WType::bytesType();
		itob->opCode = "itob";
		itob->stackArgs.push_back(std::move(_value));
		auto seven = std::make_shared<awst::IntegerConstant>();
		seven->sourceLocation = m_loc;
		seven->wtype = awst::WType::uint64Type();
		seven->value = "7";
		auto one = std::make_shared<awst::IntegerConstant>();
		one->sourceLocation = m_loc;
		one->wtype = awst::WType::uint64Type();
		one->value = "1";
		auto extract = std::make_shared<awst::IntrinsicCall>();
		extract->sourceLocation = m_loc;
		extract->wtype = awst::WType::bytesType();
		extract->opCode = "extract3";
		extract->stackArgs.push_back(std::move(itob));
		extract->stackArgs.push_back(std::move(seven));
		extract->stackArgs.push_back(std::move(one));
		_value = std::move(extract);
	}
	else if (_value->wtype && _value->wtype->kind() == awst::WTypeKind::Bytes
		&& _value->wtype != awst::WType::bytesType())
	{
		auto cast = std::make_shared<awst::ReinterpretCast>();
		cast->sourceLocation = m_loc;
		cast->wtype = awst::WType::bytesType();
		cast->expr = std::move(_value);
		_value = std::move(cast);
	}
	_value = builder::TypeCoercion::stringToBytes(std::move(_value), m_loc);

	auto replace = std::make_shared<awst::IntrinsicCall>();
	replace->sourceLocation = m_loc;
	replace->wtype = _indexExpr->base->wtype;
	replace->opCode = "replace3";
	replace->stackArgs.push_back(_indexExpr->base);
	replace->stackArgs.push_back(_indexExpr->index);
	replace->stackArgs.push_back(std::move(_value));

	auto e = std::make_shared<awst::AssignmentExpression>();
	e->sourceLocation = m_loc;
	e->wtype = _indexExpr->base->wtype;
	e->target = _indexExpr->base;
	e->value = std::move(replace);
	return e;
}

std::shared_ptr<awst::Expression> SolAssignment::handleStructFieldAssignment(
	awst::FieldExpression const* _fieldExpr,
	std::shared_ptr<awst::Expression> _value,
	std::shared_ptr<awst::Expression> _unwrappedTarget)
{
	auto const* arc4StructType = dynamic_cast<awst::ARC4Struct const*>(_fieldExpr->base->wtype);
	if (!arc4StructType)
		if (auto const* sg = dynamic_cast<awst::StateGet const*>(_fieldExpr->base.get()))
			arc4StructType = dynamic_cast<awst::ARC4Struct const*>(sg->field->wtype);
	if (!arc4StructType) return nullptr;

	Token op = m_assignment.assignmentOperator();
	auto base = _fieldExpr->base;
	std::string fieldName = _fieldExpr->name;

	if (auto const* sg = dynamic_cast<awst::StateGet const*>(base.get()))
		base = sg->field;

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

	if (op != Token::Assign)
	{
		auto currentField = std::make_shared<awst::FieldExpression>();
		currentField->sourceLocation = m_loc;
		currentField->base = readBase;
		currentField->name = fieldName;
		currentField->wtype = _fieldExpr->wtype;
		auto decoded = std::make_shared<awst::ARC4Decode>();
		decoded->sourceLocation = m_loc;
		decoded->wtype = m_ctx.typeMapper.map(m_assignment.leftHandSide().annotation().type);
		decoded->value = std::move(currentField);
		auto* solType = m_assignment.leftHandSide().annotation().type;
		auto builderResult = eb::AssignmentHelper::tryComputeCompoundValue(
			m_ctx, op, solType, decoded, _value, m_loc);
		if (builderResult)
			_value = std::move(builderResult);
		else
			_value = m_ctx.buildBinaryOp(op, std::move(decoded), std::move(_value),
				decoded->wtype, m_loc);
	}

	// ARC4Encode the value
	awst::WType const* arc4FieldType = nullptr;
	for (auto const& [fname, ftype]: arc4StructType->fields())
		if (fname == fieldName) { arc4FieldType = ftype; break; }
	if (arc4FieldType && _value->wtype != arc4FieldType)
	{
		auto encode = std::make_shared<awst::ARC4Encode>();
		encode->sourceLocation = m_loc;
		encode->wtype = arc4FieldType;
		encode->value = std::move(_value);
		_value = std::move(encode);
	}

	// Build NewStruct with copy-on-write
	auto newStruct = std::make_shared<awst::NewStruct>();
	newStruct->sourceLocation = m_loc;
	newStruct->wtype = arc4StructType;
	for (auto const& [fname, ftype]: arc4StructType->fields())
	{
		if (fname == fieldName)
			newStruct->values[fname] = std::move(_value);
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

	// Recursive copy-on-write for nested structs
	auto assignTarget2 = std::move(base);
	std::shared_ptr<awst::Expression> assignValue2 = std::move(newStruct);
	std::vector<std::pair<std::string, awst::WType const*>> fieldChain;

	while (auto const* outerField = dynamic_cast<awst::FieldExpression const*>(assignTarget2.get()))
	{
		auto const* outerStructType = dynamic_cast<awst::ARC4Struct const*>(outerField->base->wtype);
		if (!outerStructType) break;
		auto outerBase = outerField->base;
		std::string outerFieldName = outerField->name;
		awst::WType const* outerFieldWtype = nullptr;
		for (auto const& [fn, ft]: outerStructType->fields())
			if (fn == outerFieldName) { outerFieldWtype = ft; break; }
		fieldChain.push_back({outerFieldName, outerFieldWtype});

		auto outerNewStruct = std::make_shared<awst::NewStruct>();
		outerNewStruct->sourceLocation = m_loc;
		outerNewStruct->wtype = outerStructType;
		for (auto const& [fn, ft]: outerStructType->fields())
		{
			if (fn == outerFieldName)
				outerNewStruct->values[fn] = std::move(assignValue2);
			else
			{
				auto f = std::make_shared<awst::FieldExpression>();
				f->sourceLocation = m_loc;
				f->base = outerBase;
				f->name = fn;
				f->wtype = ft;
				outerNewStruct->values[fn] = std::move(f);
			}
		}
		assignTarget2 = std::move(outerBase);
		assignValue2 = std::move(outerNewStruct);
	}

	auto e = std::make_shared<awst::AssignmentExpression>();
	e->sourceLocation = m_loc;
	e->wtype = assignTarget2->wtype;
	e->target = std::move(assignTarget2);
	e->value = std::move(assignValue2);

	if (arc4FieldType)
	{
		std::shared_ptr<awst::Expression> extractBase = std::move(e);
		for (auto it = fieldChain.rbegin(); it != fieldChain.rend(); ++it)
		{
			auto fe = std::make_shared<awst::FieldExpression>();
			fe->sourceLocation = m_loc;
			fe->base = std::move(extractBase);
			fe->name = it->first;
			fe->wtype = it->second;
			extractBase = std::move(fe);
		}
		auto fieldExtract = std::make_shared<awst::FieldExpression>();
		fieldExtract->sourceLocation = m_loc;
		fieldExtract->base = std::move(extractBase);
		fieldExtract->name = fieldName;
		fieldExtract->wtype = arc4FieldType;
		auto decode = std::make_shared<awst::ARC4Decode>();
		decode->sourceLocation = m_loc;
		decode->wtype = m_ctx.typeMapper.map(m_assignment.annotation().type);
		decode->value = std::move(fieldExtract);
		return decode;
	}
	return e;
}

std::shared_ptr<awst::Expression> SolAssignment::toAwst()
{
	Token op = m_assignment.assignmentOperator();
	auto target = buildExpr(m_assignment.leftHandSide());
	auto value = buildExpr(m_assignment.rightHandSide());

	// Tuple decomposition
	if (dynamic_cast<awst::TupleExpression const*>(target.get()))
		return handleTupleAssignment(std::move(target), std::move(value));

	// Bytes element assignment: bytes[i] = value
	if (auto const* indexExpr = dynamic_cast<awst::IndexExpression const*>(target.get()))
	{
		if (indexExpr->base && indexExpr->base->wtype
			&& indexExpr->base->wtype->kind() == awst::WTypeKind::Bytes)
			return handleBytesElementAssignment(indexExpr, std::move(value));
	}

	// Unwrap ARC4Decode for Lvalue
	auto unwrappedTarget = target;
	if (auto const* decodeExpr = dynamic_cast<awst::ARC4Decode const*>(target.get()))
		unwrappedTarget = decodeExpr->value;

	// Struct field copy-on-write
	if (auto const* fieldExpr = dynamic_cast<awst::FieldExpression const*>(unwrappedTarget.get()))
	{
		auto const* arc4StructType = dynamic_cast<awst::ARC4Struct const*>(fieldExpr->base->wtype);
		if (!arc4StructType)
			if (auto const* sg = dynamic_cast<awst::StateGet const*>(fieldExpr->base.get()))
				arc4StructType = dynamic_cast<awst::ARC4Struct const*>(sg->field->wtype);
		if (arc4StructType)
			return handleStructFieldAssignment(fieldExpr, std::move(value), unwrappedTarget);

		// WTuple named field assignment
		auto const* tupleType = dynamic_cast<awst::WTuple const*>(fieldExpr->base->wtype);
		if (tupleType && tupleType->names().has_value())
		{
			auto base = fieldExpr->base;
			std::string fieldName = fieldExpr->name;

			if (op != Token::Assign)
			{
				auto currentField = std::make_shared<awst::FieldExpression>();
				currentField->sourceLocation = m_loc;
				currentField->base = base;
				currentField->name = fieldName;
				currentField->wtype = fieldExpr->wtype;
				auto* solType = m_assignment.leftHandSide().annotation().type;
				auto builderResult = eb::AssignmentHelper::tryComputeCompoundValue(
					m_ctx, op, solType, currentField, value, m_loc);
				if (builderResult)
					value = std::move(builderResult);
				else
					value = m_ctx.buildBinaryOp(op, std::move(currentField), std::move(value),
						fieldExpr->wtype, m_loc);
			}

			value = builder::TypeCoercion::implicitNumericCast(
				std::move(value), fieldExpr->wtype, m_loc);
			auto newTuple = buildTupleWithUpdatedField(base, fieldName, std::move(value));

			auto writeTarget = base;
			if (auto const* sg = dynamic_cast<awst::StateGet const*>(base.get()))
				writeTarget = sg->field;

			auto e = std::make_shared<awst::AssignmentExpression>();
			e->sourceLocation = m_loc;
			e->wtype = writeTarget->wtype;
			e->target = std::move(writeTarget);
			e->value = std::move(newTuple);

			auto fieldExtract = std::make_shared<awst::FieldExpression>();
			fieldExtract->sourceLocation = m_loc;
			fieldExtract->base = std::move(e);
			fieldExtract->name = fieldName;
			fieldExtract->wtype = fieldExpr->wtype;
			return fieldExtract;
		}
	}

	// Compound assignment
	if (op != Token::Assign)
	{
		auto currentValue = buildExpr(m_assignment.leftHandSide());
		if (dynamic_cast<awst::BoxValueExpression const*>(currentValue.get()))
		{
			auto defaultVal = builder::StorageMapper::makeDefaultValue(currentValue->wtype, m_loc);
			auto stateGet = std::make_shared<awst::StateGet>();
			stateGet->sourceLocation = m_loc;
			stateGet->wtype = currentValue->wtype;
			stateGet->field = currentValue;
			stateGet->defaultValue = defaultVal;
			currentValue = std::move(stateGet);
		}
		auto* targetSolType = m_assignment.leftHandSide().annotation().type;
		auto builderResult = eb::AssignmentHelper::tryComputeCompoundValue(
			m_ctx, op, targetSolType, currentValue, value, m_loc);
		if (builderResult)
			value = std::move(builderResult);
		else
			value = m_ctx.buildBinaryOp(op, std::move(currentValue), std::move(value),
				target->wtype, m_loc);
	}

	// Type coercion
	value = builder::TypeCoercion::implicitNumericCast(std::move(value), target->wtype, m_loc);
	if (value->wtype != target->wtype && target->wtype
		&& target->wtype->kind() == awst::WTypeKind::Bytes)
	{
		auto const* bytesType = dynamic_cast<awst::BytesWType const*>(target->wtype);
		auto const* strConst = dynamic_cast<awst::StringConstant const*>(value.get());
		if (bytesType && bytesType->length().has_value() && *bytesType->length() > 0 && strConst)
		{
			if (auto padded = builder::TypeCoercion::stringToBytesN(
					value.get(), target->wtype, *bytesType->length(), m_loc))
				value = std::move(padded);
		}
		else
		{
			bool valueIsCompatible = value->wtype == awst::WType::stringType()
				|| (value->wtype && value->wtype->kind() == awst::WTypeKind::Bytes);
			if (valueIsCompatible)
			{
				auto cast = std::make_shared<awst::ReinterpretCast>();
				cast->sourceLocation = m_loc;
				cast->wtype = target->wtype;
				cast->expr = std::move(value);
				value = std::move(cast);
			}
		}
	}
	if (value->wtype != target->wtype && target->wtype == awst::WType::stringType()
		&& value->wtype && (value->wtype->kind() == awst::WTypeKind::Bytes
			|| value->wtype == awst::WType::bytesType()))
	{
		auto cast = std::make_shared<awst::ReinterpretCast>();
		cast->sourceLocation = m_loc;
		cast->wtype = target->wtype;
		cast->expr = std::move(value);
		value = std::move(cast);
	}

	// Unwrap for assignment target
	if (auto const* decodeExpr = dynamic_cast<awst::ARC4Decode const*>(target.get()))
		target = decodeExpr->value;
	if (auto const* sg = dynamic_cast<awst::StateGet const*>(target.get()))
		target = sg->field;

	// ARC4 encode if needed
	if (value->wtype != target->wtype)
	{
		bool targetIsArc4 = false;
		switch (target->wtype->kind())
		{
		case awst::WTypeKind::ARC4UIntN:
		case awst::WTypeKind::ARC4StaticArray:
		case awst::WTypeKind::ARC4DynamicArray:
		case awst::WTypeKind::ARC4Struct:
			targetIsArc4 = true; break;
		default: break;
		}
		if (targetIsArc4)
		{
			value = builder::TypeCoercion::stringToBytes(std::move(value), m_loc);
			auto encode = std::make_shared<awst::ARC4Encode>();
			encode->sourceLocation = m_loc;
			encode->wtype = target->wtype;
			encode->value = std::move(value);
			value = std::move(encode);
		}
	}

	auto e = std::make_shared<awst::AssignmentExpression>();
	e->sourceLocation = m_loc;
	e->wtype = target->wtype;
	e->target = std::move(target);
	e->value = std::move(value);
	return e;
}

} // namespace puyasol::builder::sol_ast
