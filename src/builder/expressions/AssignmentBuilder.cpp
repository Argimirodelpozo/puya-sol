/// @file AssignmentBuilder.cpp
/// Handles assignment expressions (=, +=, -=, etc.).

#include "builder/expressions/ExpressionBuilder.h"
#include "builder/storage/StorageMapper.h"
#include "Logger.h"

#include <libsolidity/ast/TypeProvider.h>

#include <algorithm>
#include <sstream>

namespace puyasol::builder
{

bool ExpressionBuilder::visit(solidity::frontend::Assignment const& _node)
{
	using Token = solidity::frontend::Token;
	auto loc = makeLoc(_node.location());

	auto target = build(_node.leftHandSide());
	auto value = build(_node.rightHandSide());

	Token op = _node.assignmentOperator();

	// Tuple assignment decomposition: (a, b, c) = func()
	// When the LHS is a tuple expression, decompose into individual assignments
	// to avoid issues with non-lvalue items (e.g. storage targets).
	if (auto const* tupleTarget = dynamic_cast<awst::TupleExpression const*>(target.get()))
	{
		auto const& items = tupleTarget->items;
		for (size_t i = 0; i < items.size(); ++i)
		{
			auto item = items[i];

			// Extract tuple element
			auto itemExpr = std::make_shared<awst::TupleItemExpression>();
			itemExpr->sourceLocation = loc;
			itemExpr->wtype = item->wtype;
			itemExpr->base = value;
			itemExpr->index = static_cast<int>(i);

			// Unwrap ARC4Decode for storage targets
			auto assignTarget = item;
			if (auto const* decodeExpr = dynamic_cast<awst::ARC4Decode const*>(item.get()))
				assignTarget = decodeExpr->value;
			// Unwrap StateGet for storage targets
			if (auto const* sg = dynamic_cast<awst::StateGet const*>(assignTarget.get()))
				assignTarget = sg->field;

			// Encode value if target is ARC4 storage
			std::shared_ptr<awst::Expression> assignValue = std::move(itemExpr);
			if (assignTarget->wtype != assignValue->wtype)
			{
				// Need ARC4 encode for storage writes
				bool targetIsArc4 = false;
				switch (assignTarget->wtype->kind())
				{
				case awst::WTypeKind::ARC4UIntN:
				case awst::WTypeKind::ARC4StaticArray:
				case awst::WTypeKind::ARC4DynamicArray:
				case awst::WTypeKind::ARC4Struct:
					targetIsArc4 = true;
					break;
				default:
					break;
				}
				if (targetIsArc4)
				{
					auto encode = std::make_shared<awst::ARC4Encode>();
					encode->sourceLocation = loc;
					encode->wtype = assignTarget->wtype;
					encode->value = std::move(assignValue);
					assignValue = std::move(encode);
				}
				else
				{
					assignValue = implicitNumericCast(std::move(assignValue), assignTarget->wtype, loc);
				}
			}

			// Check if target is a FieldExpression on ARC4Struct — needs copy-on-write
			if (auto const* fieldExpr = dynamic_cast<awst::FieldExpression const*>(assignTarget.get()))
			{
				auto const* structType = dynamic_cast<awst::ARC4Struct const*>(fieldExpr->base->wtype);
				if (!structType)
				{
					if (auto const* sg = dynamic_cast<awst::StateGet const*>(fieldExpr->base.get()))
						structType = dynamic_cast<awst::ARC4Struct const*>(sg->field->wtype);
				}
				if (structType)
				{
					auto base = fieldExpr->base;
					std::string fName = fieldExpr->name;
					if (auto const* sg = dynamic_cast<awst::StateGet const*>(base.get()))
						base = sg->field;

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

					// ARC4Encode the value if needed
					awst::WType const* fieldType = nullptr;
					for (auto const& [fn, ft]: structType->fields())
						if (fn == fName) { fieldType = ft; break; }
					if (fieldType && assignValue->wtype != fieldType)
					{
						auto enc = std::make_shared<awst::ARC4Encode>();
						enc->sourceLocation = loc;
						enc->wtype = fieldType;
						enc->value = std::move(assignValue);
						assignValue = std::move(enc);
					}

					auto newStruct = std::make_shared<awst::NewStruct>();
					newStruct->sourceLocation = loc;
					newStruct->wtype = structType;
					for (auto const& [fn, ft]: structType->fields())
					{
						if (fn == fName)
							newStruct->values[fn] = std::move(assignValue);
						else
						{
							auto f = std::make_shared<awst::FieldExpression>();
							f->sourceLocation = loc;
							f->base = readBase;
							f->name = fn;
							f->wtype = ft;
							newStruct->values[fn] = std::move(f);
						}
					}

					auto e = std::make_shared<awst::AssignmentExpression>();
					e->sourceLocation = loc;
					e->wtype = base->wtype;
					e->target = std::move(base);
					e->value = std::move(newStruct);
					auto stmt = std::make_shared<awst::ExpressionStatement>();
					stmt->sourceLocation = loc;
					stmt->expr = e;
					m_pendingStatements.push_back(std::move(stmt));
					continue; // skip the normal assignment below
				}
			}

			// ARC4Encode value if target is ARC4-typed but value is native
			// (e.g., box array element assignment: frax_pools_array[i] = address(0))
			if (assignTarget->wtype != assignValue->wtype
				&& assignTarget->wtype->kind() == awst::WTypeKind::ARC4StaticArray)
			{
				auto enc = std::make_shared<awst::ARC4Encode>();
				enc->sourceLocation = loc;
				enc->wtype = assignTarget->wtype;
				enc->value = std::move(assignValue);
				assignValue = std::move(enc);
			}

			auto e = std::make_shared<awst::AssignmentExpression>();
			e->sourceLocation = loc;
			e->wtype = assignTarget->wtype;
			e->target = std::move(assignTarget);
			e->value = std::move(assignValue);
			auto stmt = std::make_shared<awst::ExpressionStatement>();
			stmt->sourceLocation = loc;
			stmt->expr = e;
			m_pendingStatements.push_back(std::move(stmt));
		}
		// Push a void expression as the result (this is used in statement context)
		push(value);
		return false;
	}

	// Check if target is an IndexExpression on bytes (bytes[i] = value).
	// Bytes are immutable on AVM, so transform to: base = replace3(base, i, value)
	if (auto const* indexExpr = dynamic_cast<awst::IndexExpression const*>(target.get()))
	{
		if (indexExpr->base && indexExpr->base->wtype
			&& indexExpr->base->wtype->kind() == awst::WTypeKind::Bytes)
		{
			if (op != Token::Assign)
				value = buildBinaryOp(op, build(_node.leftHandSide()), std::move(value), indexExpr->wtype, loc);

			// Coerce value to bytes if needed (e.g. bytes[1])
			if (value->wtype && value->wtype->kind() == awst::WTypeKind::Bytes
				&& value->wtype != awst::WType::bytesType())
			{
				auto cast = std::make_shared<awst::ReinterpretCast>();
				cast->sourceLocation = loc;
				cast->wtype = awst::WType::bytesType();
				cast->expr = std::move(value);
				value = std::move(cast);
			}

			auto replace = std::make_shared<awst::IntrinsicCall>();
			replace->sourceLocation = loc;
			replace->wtype = indexExpr->base->wtype;
			replace->opCode = "replace3";
			replace->stackArgs.push_back(indexExpr->base);
			replace->stackArgs.push_back(indexExpr->index);
			replace->stackArgs.push_back(std::move(value));

			auto e = std::make_shared<awst::AssignmentExpression>();
			e->sourceLocation = loc;
			e->wtype = indexExpr->base->wtype;
			e->target = indexExpr->base;
			e->value = std::move(replace);
			push(e);
			return false;
		}
	}

	// Unwrap ARC4Decode for assignment targets — ARC4Decode is not an Lvalue.
	// When assigning to a struct field, the target is ARC4Decode(FieldExpression).
	std::shared_ptr<awst::Expression> unwrappedTarget = target;
	if (auto const* decodeExpr = dynamic_cast<awst::ARC4Decode const*>(target.get()))
		unwrappedTarget = decodeExpr->value;

	// Check if target is a FieldExpression on a struct (field assignment).
	// Structs are immutable in Puya, so we use copy-on-write: build a new struct
	// with all fields copied except the modified one, then assign the whole struct.
	if (auto const* fieldExpr = dynamic_cast<awst::FieldExpression const*>(unwrappedTarget.get()))
	{
		// ARC4Struct field assignment: copy-on-write with NewStruct
		// Detect ARC4Struct type on the base — base may be BoxValueExpression or StateGet(BoxValueExpression)
		Logger::instance().debug(
			"DEBUG: field assign target '" + fieldExpr->name
			+ "' base._type=" + fieldExpr->base->nodeType()
			+ " base.wtype=" + (fieldExpr->base->wtype ? fieldExpr->base->wtype->name() : "null"),
			loc);
		auto const* arc4StructType = dynamic_cast<awst::ARC4Struct const*>(fieldExpr->base->wtype);
		if (!arc4StructType)
		{
			// If base is StateGet, check the inner field's wtype
			if (auto const* sg = dynamic_cast<awst::StateGet const*>(fieldExpr->base.get()))
				arc4StructType = dynamic_cast<awst::ARC4Struct const*>(sg->field->wtype);
		}
		if (arc4StructType)
		{
			auto base = fieldExpr->base;
			std::string fieldName = fieldExpr->name;

			// Unwrap StateGet to get the underlying BoxValueExpression for both read and write
			if (auto const* sg = dynamic_cast<awst::StateGet const*>(base.get()))
				base = sg->field;

			// Ensure base is readable for field extraction.
			// If base is a bare BoxValueExpression (e.g. direct _mapping[key].field = value
			// with willBeWrittenTo=true), wrap in StateGet for reading.
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

			if (op != Token::Assign)
			{
				auto currentField = std::make_shared<awst::FieldExpression>();
				currentField->sourceLocation = loc;
				currentField->base = readBase;
				currentField->name = fieldName;
				currentField->wtype = fieldExpr->wtype;
				// Decode the current field value for the binary op (it's ARC4)
				auto decoded = std::make_shared<awst::ARC4Decode>();
				decoded->sourceLocation = loc;
				decoded->wtype = m_typeMapper.map(_node.leftHandSide().annotation().type);
				decoded->value = std::move(currentField);
				value = buildBinaryOp(op, std::move(decoded), std::move(value), decoded->wtype, loc);
			}

			// Wrap value in ARC4Encode to match the ARC4 field type
			awst::WType const* arc4FieldType = nullptr;
			for (auto const& [fname, ftype]: arc4StructType->fields())
				if (fname == fieldName)
				{
					arc4FieldType = ftype;
					break;
				}
			if (arc4FieldType && value->wtype != arc4FieldType)
			{
				auto encode = std::make_shared<awst::ARC4Encode>();
				encode->sourceLocation = loc;
				encode->wtype = arc4FieldType;
				encode->value = std::move(value);
				value = std::move(encode);
			}

			// Build NewStruct with all fields copied, replacing the modified one
			auto newStruct = std::make_shared<awst::NewStruct>();
			newStruct->sourceLocation = loc;
			newStruct->wtype = arc4StructType;
			for (auto const& [fname, ftype]: arc4StructType->fields())
			{
				if (fname == fieldName)
					newStruct->values[fname] = std::move(value);
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

			auto e = std::make_shared<awst::AssignmentExpression>();
			e->sourceLocation = loc;
			e->wtype = writeTarget->wtype;
			e->target = std::move(writeTarget);
			e->value = std::move(newStruct);

			// In Solidity, (struct.field = val) evaluates to the assigned field
			// value, not the whole struct. Wrap in FieldExpression + ARC4Decode
			// to extract the field value from the struct assignment result.
			if (arc4FieldType)
			{
				auto fieldExtract = std::make_shared<awst::FieldExpression>();
				fieldExtract->sourceLocation = loc;
				fieldExtract->base = std::move(e);
				fieldExtract->name = fieldName;
				fieldExtract->wtype = arc4FieldType;

				auto decode = std::make_shared<awst::ARC4Decode>();
				decode->sourceLocation = loc;
				decode->wtype = m_typeMapper.map(_node.annotation().type);
				decode->value = std::move(fieldExtract);
				push(decode);
			}
			else
				push(e);
			return false;
		}

		auto const* tupleType = dynamic_cast<awst::WTuple const*>(fieldExpr->base->wtype);
		if (tupleType && tupleType->names().has_value())
		{
			auto base = fieldExpr->base;
			std::string fieldName = fieldExpr->name;

			if (op != Token::Assign)
			{
				// Compound assignment: read current field value for the binary op
				auto currentField = std::make_shared<awst::FieldExpression>();
				currentField->sourceLocation = loc;
				currentField->base = base;
				currentField->name = fieldName;
				currentField->wtype = fieldExpr->wtype;
				value = buildBinaryOp(op, std::move(currentField), std::move(value), fieldExpr->wtype, loc);
			}

			// Cast value to match the field's type
			value = implicitNumericCast(std::move(value), fieldExpr->wtype, loc);

			auto newTuple = buildTupleWithUpdatedField(base, fieldName, std::move(value), loc);

			// For the assignment target, unwrap StateGet to get the underlying
			// StorageExpression (e.g. BoxValueExpression). StateGet is only valid
			// for reads, not as an Lvalue.
			auto writeTarget = base;
			if (auto const* sg = dynamic_cast<awst::StateGet const*>(base.get()))
				writeTarget = sg->field;

			auto e = std::make_shared<awst::AssignmentExpression>();
			e->sourceLocation = loc;
			e->wtype = writeTarget->wtype;
			e->target = std::move(writeTarget);
			e->value = std::move(newTuple);

			// In Solidity, (struct.field = val) evaluates to the assigned field
			// value, not the whole struct. Extract the field from the result.
			auto fieldExtract = std::make_shared<awst::FieldExpression>();
			fieldExtract->sourceLocation = loc;
			fieldExtract->base = std::move(e);
			fieldExtract->name = fieldName;
			fieldExtract->wtype = fieldExpr->wtype;
			push(fieldExtract);
			return false;
		}
	}

	if (op != Token::Assign)
	{
		// Compound assignment: target op= value → target = target op value
		auto currentValue = build(_node.leftHandSide());
		// For box-stored mappings, the re-translated LHS is a BoxValueExpression
		// (no StateGet default) because willBeWrittenTo=true. Wrap it in StateGet
		// so missing boxes return the default value instead of asserting existence.
		if (dynamic_cast<awst::BoxValueExpression const*>(currentValue.get()))
		{
			auto defaultVal = StorageMapper::makeDefaultValue(currentValue->wtype, loc);
			auto stateGet = std::make_shared<awst::StateGet>();
			stateGet->sourceLocation = loc;
			stateGet->wtype = currentValue->wtype;
			stateGet->field = currentValue;
			stateGet->defaultValue = defaultVal;
			currentValue = std::move(stateGet);
		}
		value = buildBinaryOp(op, std::move(currentValue), std::move(value), target->wtype, loc);
	}

	// Insert implicit numeric cast if value type differs from target type
	value = implicitNumericCast(std::move(value), target->wtype, loc);
	// Coerce between bytes-compatible types (string → bytes, bytes → bytes[N], etc.)
	if (value->wtype != target->wtype
		&& target->wtype && target->wtype->kind() == awst::WTypeKind::Bytes)
	{
		bool valueIsCompatible = value->wtype == awst::WType::stringType()
			|| (value->wtype && value->wtype->kind() == awst::WTypeKind::Bytes);
		if (valueIsCompatible)
		{
			auto cast = std::make_shared<awst::ReinterpretCast>();
			cast->sourceLocation = loc;
			cast->wtype = target->wtype;
			cast->expr = std::move(value);
			value = std::move(cast);
		}
	}
	// Coerce bytes → string when target is string (e.g., b = hex"41424344" where b is string)
	if (value->wtype != target->wtype
		&& target->wtype == awst::WType::stringType()
		&& value->wtype && (value->wtype->kind() == awst::WTypeKind::Bytes
			|| value->wtype == awst::WType::bytesType()))
	{
		auto cast = std::make_shared<awst::ReinterpretCast>();
		cast->sourceLocation = loc;
		cast->wtype = target->wtype;
		cast->expr = std::move(value);
		value = std::move(cast);
	}

	// Unwrap ARC4Decode for assignment targets — ARC4Decode is not an Lvalue.
	if (auto const* decodeExpr = dynamic_cast<awst::ARC4Decode const*>(target.get()))
		target = decodeExpr->value;

	// Unwrap StateGet for the assignment target — StateGet is only valid for reads,
	// not as an Lvalue. Use the underlying StorageExpression instead.
	if (auto const* sg = dynamic_cast<awst::StateGet const*>(target.get()))
		target = sg->field;

	// ARC4Encode value if target is ARC4-typed but value is native
	// (e.g., box array element: frax_pools_array[i] = address(0))
	if (value->wtype != target->wtype)
	{
		bool targetIsArc4 = false;
		switch (target->wtype->kind())
		{
		case awst::WTypeKind::ARC4UIntN:
		case awst::WTypeKind::ARC4StaticArray:
		case awst::WTypeKind::ARC4DynamicArray:
		case awst::WTypeKind::ARC4Struct:
			targetIsArc4 = true;
			break;
		default:
			break;
		}
		if (targetIsArc4)
		{
			auto encode = std::make_shared<awst::ARC4Encode>();
			encode->sourceLocation = loc;
			encode->wtype = target->wtype;
			encode->value = std::move(value);
			value = std::move(encode);
		}
	}

	auto e = std::make_shared<awst::AssignmentExpression>();
	e->sourceLocation = loc;
	e->wtype = target->wtype;
	e->target = std::move(target);
	e->value = std::move(value);
	push(e);
	return false;
}


} // namespace puyasol::builder
