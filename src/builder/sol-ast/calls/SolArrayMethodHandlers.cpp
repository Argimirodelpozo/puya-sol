/// @file SolArrayMethodHandlers.cpp
/// Per-array-kind handlers for `array.push() / pop() / length()`:
/// struct-field arrays, box-backed arrays, memory arrays, and the
/// mapping-element-array length-only box. Extracted from
/// SolArrayMethod.cpp; the dispatcher there (`SolArrayMethod::toAwst`)
/// routes each array shape to the corresponding handler below.

#include "builder/sol-ast/calls/SolArrayMethod.h"
#include "awst/NameGen.h"
#include "builder/storage/StorageMapper.h"
#include "builder/sol-types/TypeMapper.h"
#include "builder/sol-types/TypeCoercion.h"

#include <libsolidity/ast/AST.h>

namespace puyasol::builder::sol_ast
{

using namespace solidity::frontend;

std::shared_ptr<awst::Expression> SolArrayMethod::handleStructFieldArrayMethod(
	std::string const& _memberName,
	MemberAccess const& _fieldAccess,
	VariableDeclaration const& _structVar)
{
	std::string fieldName = _fieldAccess.memberName();
	auto binding = m_ctx.storageMapper.physicalBindingFor(_structVar);
	std::string varName = binding.name;
	auto loc = m_loc;

	// Determine the field's array type and element type.
	auto const* structType = dynamic_cast<StructType const*>(_structVar.type());
	if (!structType)
		return nullptr;
	auto const& structDef = structType->structDefinition();

	ArrayType const* fieldArrayType = nullptr;
	for (auto const& member : structDef.members())
	{
		if (member->name() == fieldName)
		{
			fieldArrayType = dynamic_cast<ArrayType const*>(member->type());
			break;
		}
	}
	if (!fieldArrayType)
		return nullptr;

	auto* structWType = m_ctx.typeMapper.map(_structVar.type());
	// Use the field's ACTUAL type from the mapped struct, not a fresh map() of the
	// array type. For a recursive array field (`S[] x` inside S), the struct holds
	// ARC4DynamicArray<projection>, while a fresh map(S[]) re-derives the full
	// recursive S → element-type mismatch at the push. For non-recursive fields the
	// two are identical (no change).
	awst::WType const* rawFieldType = awst::structFieldType(structWType, fieldName);
	if (!rawFieldType)
		rawFieldType = m_ctx.typeMapper.map(fieldArrayType);
	awst::WType const* elemType = nullptr;
	if (auto const* da = dynamic_cast<awst::ARC4DynamicArray const*>(rawFieldType))
		elemType = da->elementType();
	else if (auto const* sa = dynamic_cast<awst::ARC4StaticArray const*>(rawFieldType))
		elemType = sa->elementType();
	if (!elemType)
		elemType = m_ctx.typeMapper.mapSolTypeToARC4(fieldArrayType->baseType());
	auto kind = binding.kind;

	// Read the struct (box_get or app_global_get with default).
	auto structRead = m_ctx.storageMapper.createStateRead(
		varName, structWType, kind, loc);

	// tmp = structRead
	std::string tmpName = "__struct_arr_tmp_" + std::to_string(awst::NameGen::next("SolArrayMethodHandlers.structPushCounter"));
	auto tmpTarget = awst::makeVarExpression(tmpName, structWType, loc);
	m_ctx.queuePostEffect(awst::makeAssignmentStatement(tmpTarget, std::move(structRead), loc));

	// tmp.field (FieldExpression)
	auto tmpRead = awst::makeVarExpression(tmpName, structWType, loc);
	auto fieldExpr = awst::makeFieldExpression(std::move(tmpRead), fieldName, rawFieldType, loc);

	// Mutate tmp.field via ArrayExtend / ArrayPop
	if (_memberName == "push")
	{
		std::shared_ptr<awst::Expression> val;
		if (!m_call.arguments().empty())
		{
			val = buildExpr(*m_call.arguments()[0]);
			// ARC4-encode the value if the element type is ARC4
			if (elemType && val->wtype != elemType)
			{
				val = builder::TypeCoercion::implicitNumericCast(
					std::move(val), elemType, loc);
				if (val->wtype != elemType)
					val = awst::makeARC4Encode(std::move(val), elemType, loc);
			}
		}
		else
			val = builder::TypeCoercion::makeDefaultValue(elemType, loc);

		m_ctx.queuePostExpression(awst::makeArrayPushOne(fieldExpr, std::move(val), rawFieldType, loc), loc);
	}
	else // pop
		m_ctx.queuePostExpression(awst::makeArrayPop(fieldExpr, elemType ? elemType : rawFieldType, loc), loc);

	// Write the struct back (box_put or app_global_put)
	auto tmpWriteRead = awst::makeVarExpression(tmpName, structWType, loc);

	auto writeExpr = m_ctx.storageMapper.createStateWrite(
		varName, std::move(tmpWriteRead), structWType, kind, loc);
	if (writeExpr)
		m_ctx.queuePostExpression(std::move(writeExpr), loc);

	return awst::makeVoidConstant(loc);
}

std::shared_ptr<awst::Expression> SolArrayMethod::handleBoxArray(
	std::string const& _memberName,
	Expression const& _baseExpr,
	VariableDeclaration const& _varDecl,
	std::shared_ptr<awst::Expression> _runtimeKey)
{
	auto const* solArrType = dynamic_cast<ArrayType const*>(_varDecl.type());
	auto* rawElemType = m_ctx.typeMapper.map(solArrType->baseType());
	auto* elemType = m_ctx.typeMapper.mapSolTypeToARC4(solArrType->baseType());
	auto* arrWType = m_ctx.typeMapper.map(solArrType);

	// Physical binding, not the raw source name — matches every other
	// box-key derivation for this declaration (colliding names diverge).
	std::string arrayVarName =
		m_ctx.storageMapper.physicalBindingFor(_varDecl).name;

	// `mapping(K=>V)[] a`: no element bytes inline; array box is just a
	// 2-byte length header. `a[i][k]` boxes are derived from `a`+`i`+sha256(k)
	// (SolIndexAccess). Gives EVM's "delete leaves data at hash" for free.
	bool elemIsMapping = dynamic_cast<MappingType const*>(solArrType->baseType()) != nullptr;
	if (elemIsMapping && (_memberName == "push" || _memberName == "pop"))
		return handleMappingElementArrayLengthOp(
			_memberName, _varDecl, arrayVarName, std::move(_runtimeKey));

	// Build BoxValueExpression
	auto boxExpr = _runtimeKey
		? awst::makeBoxValueExpression(std::move(_runtimeKey), arrWType, m_loc)
		: builder::StorageMapper::makeTopLevelBoxExpr(arrayVarName, arrWType, m_loc);

	// StateGet wrapper for reads (returns empty array if box missing)
	auto emptyArr = awst::makeNewArray(arrWType, m_loc);

	auto stateGet = awst::makeStateGet(boxExpr, emptyArr, arrWType, m_loc);

	std::shared_ptr<awst::Expression> writeExpr = boxExpr;

	if (_memberName == "push" && !m_call.arguments().empty())
	{
		auto val = buildExpr(*m_call.arguments()[0]);
		auto encoded = awst::makeARC4Encode(std::move(val), elemType, m_loc);
		return awst::makeArrayPushOne(writeExpr, std::move(encoded), arrWType, m_loc);
	}
	else if (_memberName == "push" && m_call.arguments().empty())
	{
		// push() with no args: ArrayExtend with a zero-valued element (puya
		// handles ARC4 length header; manual box_resize doesn't). When
		// SolAssignment scoped an explicit push-assignment value; use it and return the
		// extend directly (VoidConstant as assignment target is rejected by puya).
		std::shared_ptr<awst::Expression> elem;
		bool fromAssign = m_ctx.hasArrayAssignmentValue();
		if (fromAssign)
		{
			auto coerced = builder::TypeCoercion::coerceForAssignment(
				m_ctx.takeArrayAssignmentValue(), rawElemType, m_loc);
			elem = awst::makeARC4Encode(std::move(coerced), elemType, m_loc);
		}
		else
		{
			elem = builder::TypeCoercion::makeDefaultValue(elemType, m_loc);
		}

		auto e = awst::makeArrayPushOne(writeExpr, std::move(elem), arrWType, m_loc);

		if (fromAssign)
			return e;

		m_ctx.queuePostExpression(std::move(e), m_loc);
		return awst::makeVoidConstant(m_loc);
	}
	else if (_memberName == "pop")
		return awst::makeArrayPopDecode(writeExpr, elemType, rawElemType, m_loc);

	return awst::makeVoidConstant(m_loc);
}

std::shared_ptr<awst::Expression> SolArrayMethod::handleMemoryArray(
	std::string const& _memberName,
	Expression const& _baseExpr)
{
	auto base = buildExpr(_baseExpr);

	if (_memberName == "push" && !m_call.arguments().empty())
	{
		auto val = buildExpr(*m_call.arguments()[0]);
		auto* baseWtype = base->wtype;

		// For bytes/string types, push is concat(base, byte)
		if (baseWtype == awst::WType::bytesType()
			|| baseWtype == awst::WType::stringType()
			|| (baseWtype && baseWtype->kind() == awst::WTypeKind::Bytes))
		{
			auto byteVal = val;
			if (byteVal->wtype == awst::WType::uint64Type())
			{
				auto itob = awst::makeItob(std::move(byteVal), m_loc);

				auto seven = awst::makeIntegerConstant("7", m_loc);
				auto one = awst::makeOne(m_loc);

				auto extract = awst::makeExtract3(std::move(itob), std::move(seven), std::move(one), m_loc);
				byteVal = std::move(extract);
			}
			else if (byteVal->wtype != awst::WType::bytesType())
			{
				byteVal = builder::TypeCoercion::stringToBytes(std::move(byteVal), m_loc);
				if (byteVal->wtype != awst::WType::bytesType())
				{
					auto cast = awst::makeAsBytes(std::move(byteVal), m_loc);
					byteVal = std::move(cast);
				}
			}

			auto cat = awst::makeIntrinsicCall("concat", baseWtype, m_loc);
			cat->stackArgs.push_back(std::move(base));
			cat->stackArgs.push_back(std::move(byteVal));
			return cat;
		}
		else
		{
			// array.push(val) — ArrayExtend
			// Get element type from array and coerce/encode value
			awst::WType const* elemType = nullptr;
			if (auto const* refArr = dynamic_cast<awst::ReferenceArray const*>(baseWtype))
				elemType = refArr->elementType();
			else if (auto const* arc4Static = dynamic_cast<awst::ARC4StaticArray const*>(baseWtype))
				elemType = arc4Static->elementType();
			else if (auto const* arc4Dyn = dynamic_cast<awst::ARC4DynamicArray const*>(baseWtype))
				elemType = arc4Dyn->elementType();

			if (elemType && val->wtype != elemType)
			{
				// Try numeric cast first (e.g., uint64 → biguint)
				val = builder::TypeCoercion::implicitNumericCast(
					std::move(val), elemType, m_loc);
				// ARC4Encode if still mismatched (native → ARC4)
				if (val->wtype != elemType)
				{
					auto encode = awst::makeARC4Encode(std::move(val), elemType, m_loc);
					val = std::move(encode);
				}
			}
			return awst::makeArrayPushOne(std::move(base), std::move(val), baseWtype, m_loc);
		}
	}
	else if (_memberName == "pop")
	{
		return awst::makeArrayPop(std::move(base), awst::WType::voidType(), m_loc);
	}

	return awst::makeVoidConstant(m_loc);
}

std::shared_ptr<awst::Expression> SolArrayMethod::handleMappingElementArrayLengthOp(
	std::string const& _memberName,
	solidity::frontend::VariableDeclaration const& /*_varDecl*/,
	std::string const& _arrayVarName,
	std::shared_ptr<awst::Expression> _runtimeKey)
{
	// Box: 2-byte big-endian length, no element data.
	// Read: box_get; empty (deleted/never-created) → len=0; else extract_uint16(0).
	// Write: itob(new_len) → extract last 2 bytes → box_put.

	auto boxKey = _runtimeKey ? std::move(_runtimeKey)
		: awst::makeUtf8BytesConstant(
			_arrayVarName, m_loc, awst::WType::boxKeyType());

	// Read box bytes (or empty if missing).
	auto boxRead = [&]() -> std::shared_ptr<awst::Expression> {
		auto box = awst::makeBoxValueExpression(boxKey, awst::WType::bytesType(), m_loc);
		return awst::makeStateGet(box, awst::makeBytesConstant({}, m_loc), awst::WType::bytesType(), m_loc);
	};

	// currentLen = box_bytes.length() > 0 ? extract_uint16(box,0) : 0
	// (exists → len>=2; guard to avoid extract_uint16 on empty bytes)
	auto bytes = boxRead();
	auto lenOfBytes = awst::makeLen(bytes, m_loc);
	auto isNonEmpty = awst::makeNumericCompare(
		std::move(lenOfBytes),
		awst::NumericComparison::Gt,
		awst::makeIntegerConstant("0", m_loc),
		m_loc);
	auto extractLen = awst::makeExtractUInt16(
		boxRead(), awst::makeZero(m_loc), m_loc);
	auto cur = awst::makeConditional(
		std::move(isNonEmpty), std::move(extractLen),
		awst::makeIntegerConstant("0", m_loc),
		awst::WType::uint64Type(), m_loc);

	// new_len = cur ± 1
	auto delta = awst::makeOne(m_loc);
	auto newLen = awst::makeUInt64BinOp(
		std::move(cur),
		_memberName == "push" ? awst::UInt64BinaryOperator::Add
							  : awst::UInt64BinaryOperator::Sub,
		std::move(delta), m_loc);

	auto extract = awst::makeUInt16Bytes(std::move(newLen), m_loc);
	// box_put(arrayVarName, len_bytes)
	m_ctx.queuePostExpression(awst::makeBoxPut(boxKey, std::move(extract), m_loc), m_loc);
	return awst::makeVoidConstant(m_loc);
}

} // namespace puyasol::builder::sol_ast
