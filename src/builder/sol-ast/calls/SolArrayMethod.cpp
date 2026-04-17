/// @file SolArrayMethod.cpp
/// array.push(val), array.push(), and array.pop().
/// Box-backed arrays read/write from box storage; memory arrays use AWST nodes directly.

#include "builder/sol-ast/calls/SolArrayMethod.h"
#include "builder/storage/StorageMapper.h"
#include "builder/sol-types/TypeMapper.h"
#include "builder/sol-types/TypeCoercion.h"

#include <libsolidity/ast/AST.h>

namespace puyasol::builder::sol_ast
{

using namespace solidity::frontend;

std::shared_ptr<awst::Expression> SolArrayMethod::toAwst()
{
	auto const& funcExpr = funcExpression();
	auto const* memberAccess = dynamic_cast<MemberAccess const*>(&funcExpr);
	if (!memberAccess)
		return nullptr;

	std::string memberName = memberAccess->memberName();
	auto const& baseExpr = memberAccess->expression();

	// Check if this is a state variable array
	if (auto const* ident = dynamic_cast<Identifier const*>(&baseExpr))
	{
		if (auto const* varDecl = dynamic_cast<VariableDeclaration const*>(
				ident->annotation().referencedDeclaration))
		{
			// bytes/string state variable: pop = read + substring + write
			if (varDecl->isStateVariable()
				&& varDecl->type()->category() == Type::Category::Array)
			{
				auto const* arrType2 = dynamic_cast<ArrayType const*>(varDecl->type());
				if (arrType2 && arrType2->isByteArrayOrString() && memberName == "pop")
				{
					std::string varName = varDecl->name();
					auto loc = m_loc;
					auto kind = builder::StorageMapper::shouldUseBoxStorage(*varDecl)
						? awst::AppStorageKind::Box
						: awst::AppStorageKind::AppGlobal;

					// Read current value
					auto readVal = m_ctx.storageMapper.createStateRead(
						varName, awst::WType::bytesType(), kind, loc);

					// len - 1
					auto lenCall = std::make_shared<awst::IntrinsicCall>();
					lenCall->sourceLocation = loc;
					lenCall->wtype = awst::WType::uint64Type();
					lenCall->opCode = "len";
					lenCall->stackArgs.push_back(readVal);

					auto one = awst::makeIntegerConstant("1", loc);
					auto newLen = std::make_shared<awst::UInt64BinaryOperation>();
					newLen->sourceLocation = loc;
					newLen->wtype = awst::WType::uint64Type();
					newLen->left = std::move(lenCall);
					newLen->op = awst::UInt64BinaryOperator::Sub;
					newLen->right = std::move(one);

					// extract3(readVal, 0, len-1)
					auto zero = awst::makeIntegerConstant("0", loc);

					auto extract = std::make_shared<awst::IntrinsicCall>();
					extract->sourceLocation = loc;
					extract->wtype = awst::WType::bytesType();
					extract->opCode = "extract3";
					extract->stackArgs.push_back(readVal);
					extract->stackArgs.push_back(std::move(zero));
					extract->stackArgs.push_back(std::move(newLen));

					if (kind == awst::AppStorageKind::Box)
					{
						// Box: store shrunk in temp, box_del, box_put
						static int popTmpCounter = 0;
						std::string tmpName = "__bytes_pop_tmp_" + std::to_string(popTmpCounter++);

						auto tmpTarget = awst::makeVarExpression(tmpName, awst::WType::bytesType(), loc);
						auto assignTmp = std::make_shared<awst::AssignmentStatement>();
						assignTmp->sourceLocation = loc;
						assignTmp->target = tmpTarget;
						assignTmp->value = std::move(extract);
						m_ctx.pendingStatements.push_back(std::move(assignTmp));

						auto del = std::make_shared<awst::IntrinsicCall>();
						del->sourceLocation = loc;
						del->wtype = awst::WType::boolType();
						del->opCode = "box_del";
						del->stackArgs.push_back(awst::makeUtf8BytesConstant(varName, loc));
						auto delStmt = awst::makeExpressionStatement(std::move(del), loc);
						m_ctx.pendingStatements.push_back(std::move(delStmt));

						auto tmpRead = awst::makeVarExpression(tmpName, awst::WType::bytesType(), loc);
						auto put = std::make_shared<awst::IntrinsicCall>();
						put->sourceLocation = loc;
						put->wtype = awst::WType::voidType();
						put->opCode = "box_put";
						put->stackArgs.push_back(awst::makeUtf8BytesConstant(varName, loc));
						put->stackArgs.push_back(std::move(tmpRead));
						auto putStmt = awst::makeExpressionStatement(std::move(put), loc);
						m_ctx.pendingStatements.push_back(std::move(putStmt));
					}
					else
					{
						auto put = std::make_shared<awst::IntrinsicCall>();
						put->sourceLocation = loc;
						put->wtype = awst::WType::voidType();
						put->opCode = "app_global_put";
						put->stackArgs.push_back(awst::makeUtf8BytesConstant(varName, loc));
						put->stackArgs.push_back(std::move(extract));
						auto stmt = awst::makeExpressionStatement(std::move(put), loc);
						m_ctx.pendingStatements.push_back(std::move(stmt));
					}

					auto vc = std::make_shared<awst::VoidConstant>();
					vc->sourceLocation = loc;
					vc->wtype = awst::WType::voidType();
					return vc;
				}
			}

			// bytes/string state variable: push = read + concat + write
			// Must come BEFORE generic box array handler since bytes in box
			// needs concat-based push, not element-by-element box array ops.
			if (varDecl->isStateVariable()
				&& varDecl->type()->category() == Type::Category::Array)
			{
				auto const* arrType = dynamic_cast<ArrayType const*>(varDecl->type());
				if (arrType && arrType->isByteArrayOrString() && memberName == "push")
				{
					std::string varName = varDecl->name();
					auto loc = m_loc;
					auto kind = builder::StorageMapper::shouldUseBoxStorage(*varDecl)
						? awst::AppStorageKind::Box
						: awst::AppStorageKind::AppGlobal;

					// Read current value
					auto readVal = m_ctx.storageMapper.createStateRead(
						varName, awst::WType::bytesType(), kind, loc);

					// Build the push value
					std::shared_ptr<awst::Expression> pushVal;
					if (!m_call.arguments().empty())
					{
						pushVal = buildExpr(*m_call.arguments()[0]);
						pushVal = builder::TypeCoercion::stringToBytes(std::move(pushVal), loc);
					}
					else
					{
						pushVal = awst::makeBytesConstant({0}, loc);
					}

					// concat(current, pushVal)
					auto cat = std::make_shared<awst::IntrinsicCall>();
					cat->sourceLocation = loc;
					cat->wtype = awst::WType::bytesType();
					cat->opCode = "concat";
					cat->stackArgs.push_back(std::move(readVal));
					cat->stackArgs.push_back(std::move(pushVal));

					if (kind == awst::AppStorageKind::Box)
					{
						// Box: store concat in temp, box_del, box_put(key, temp)
						// box_put requires exact size match, so we delete+recreate
						static int tmpCounter = 0;
						std::string tmpName = "__bytes_push_tmp_" + std::to_string(tmpCounter++);

						auto tmpTarget = awst::makeVarExpression(tmpName, awst::WType::bytesType(), loc);

						auto assignTmp = std::make_shared<awst::AssignmentStatement>();
						assignTmp->sourceLocation = loc;
						assignTmp->target = tmpTarget;
						assignTmp->value = std::move(cat);
						m_ctx.pendingStatements.push_back(std::move(assignTmp));

						auto del = std::make_shared<awst::IntrinsicCall>();
						del->sourceLocation = loc;
						del->wtype = awst::WType::boolType();
						del->opCode = "box_del";
						del->stackArgs.push_back(awst::makeUtf8BytesConstant(varName, loc));
						auto delStmt = awst::makeExpressionStatement(std::move(del), loc);
						m_ctx.pendingStatements.push_back(std::move(delStmt));

						auto tmpRead = awst::makeVarExpression(tmpName, awst::WType::bytesType(), loc);
						auto put = std::make_shared<awst::IntrinsicCall>();
						put->sourceLocation = loc;
						put->wtype = awst::WType::voidType();
						put->opCode = "box_put";
						put->stackArgs.push_back(awst::makeUtf8BytesConstant(varName, loc));
						put->stackArgs.push_back(std::move(tmpRead));
						auto putStmt = awst::makeExpressionStatement(std::move(put), loc);
						m_ctx.pendingStatements.push_back(std::move(putStmt));
					}
					else
					{
						auto put = std::make_shared<awst::IntrinsicCall>();
						put->sourceLocation = loc;
						put->wtype = awst::WType::voidType();
						put->opCode = "app_global_put";
						put->stackArgs.push_back(awst::makeUtf8BytesConstant(varName, loc));
						put->stackArgs.push_back(std::move(cat));
						auto stmt = awst::makeExpressionStatement(std::move(put), loc);
						m_ctx.pendingStatements.push_back(std::move(stmt));
					}

					auto vc = std::make_shared<awst::VoidConstant>();
					vc->sourceLocation = loc;
					vc->wtype = awst::WType::voidType();
					return vc;
				}
			}

			// Generic box-stored dynamic array (non-bytes)
			if (varDecl->isStateVariable()
				&& builder::StorageMapper::shouldUseBoxStorage(*varDecl)
				&& dynamic_cast<ArrayType const*>(varDecl->type()))
			{
				return handleBoxArray(memberName, baseExpr, *varDecl);
			}
		}
	}

	// Struct-field array push/pop: `s.b.push(val)` where s is a storage
	// struct and b is a dynamic array field. Emit copy-on-write: read the
	// struct into a temp, mutate tmp.b in place, write the struct back.
	if (auto const* innerMA = dynamic_cast<MemberAccess const*>(&baseExpr))
	{
		if (auto const* outerIdent = dynamic_cast<Identifier const*>(
				&innerMA->expression()))
		{
			if (auto const* outerVar = dynamic_cast<VariableDeclaration const*>(
					outerIdent->annotation().referencedDeclaration))
			{
				if (outerVar->isStateVariable()
					&& outerVar->type()->category() == Type::Category::Struct
					&& (memberName == "push" || memberName == "pop"))
				{
					return handleStructFieldArrayMethod(
						memberName, *innerMA, *outerVar);
				}
			}
		}
	}

	return handleMemoryArray(memberName, baseExpr);
}

std::shared_ptr<awst::Expression> SolArrayMethod::handleStructFieldArrayMethod(
	std::string const& _memberName,
	MemberAccess const& _fieldAccess,
	VariableDeclaration const& _structVar)
{
	std::string fieldName = _fieldAccess.memberName();
	std::string varName = _structVar.name();
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
	auto* rawFieldType = m_ctx.typeMapper.map(fieldArrayType);
	auto* elemType = m_ctx.typeMapper.mapSolTypeToARC4(fieldArrayType->baseType());
	auto kind = builder::StorageMapper::shouldUseBoxStorage(_structVar)
		? awst::AppStorageKind::Box
		: awst::AppStorageKind::AppGlobal;

	// Read the struct (box_get or app_global_get with default).
	auto structRead = m_ctx.storageMapper.createStateRead(
		varName, structWType, kind, loc);

	// tmp = structRead
	static int structPushCounter = 0;
	std::string tmpName = "__struct_arr_tmp_" + std::to_string(structPushCounter++);
	auto tmpTarget = awst::makeVarExpression(tmpName, structWType, loc);
	auto tmpAssign = std::make_shared<awst::AssignmentStatement>();
	tmpAssign->sourceLocation = loc;
	tmpAssign->target = tmpTarget;
	tmpAssign->value = std::move(structRead);
	m_ctx.pendingStatements.push_back(std::move(tmpAssign));

	// tmp.field (FieldExpression)
	auto tmpRead = awst::makeVarExpression(tmpName, structWType, loc);
	auto fieldExpr = std::make_shared<awst::FieldExpression>();
	fieldExpr->sourceLocation = loc;
	fieldExpr->wtype = rawFieldType;
	fieldExpr->base = std::move(tmpRead);
	fieldExpr->name = fieldName;

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
				{
					auto encode = std::make_shared<awst::ARC4Encode>();
					encode->sourceLocation = loc;
					encode->wtype = elemType;
					encode->value = std::move(val);
					val = std::move(encode);
				}
			}
		}
		else
		{
			val = builder::TypeCoercion::makeDefaultValue(elemType, loc);
		}

		auto singleArr = std::make_shared<awst::NewArray>();
		singleArr->sourceLocation = loc;
		singleArr->wtype = rawFieldType;
		singleArr->values.push_back(std::move(val));

		auto extend = std::make_shared<awst::ArrayExtend>();
		extend->sourceLocation = loc;
		extend->wtype = awst::WType::voidType();
		extend->base = fieldExpr;
		extend->other = std::move(singleArr);
		auto extendStmt = awst::makeExpressionStatement(std::move(extend), loc);
		m_ctx.pendingStatements.push_back(std::move(extendStmt));
	}
	else // pop
	{
		auto popExpr = std::make_shared<awst::ArrayPop>();
		popExpr->sourceLocation = loc;
		popExpr->wtype = elemType ? elemType : rawFieldType;
		popExpr->base = fieldExpr;
		auto popStmt = awst::makeExpressionStatement(std::move(popExpr), loc);
		m_ctx.pendingStatements.push_back(std::move(popStmt));
	}

	// Write the struct back (box_put or app_global_put)
	auto tmpWriteRead = awst::makeVarExpression(tmpName, structWType, loc);

	auto writeExpr = m_ctx.storageMapper.createStateWrite(
		varName, std::move(tmpWriteRead), structWType, kind, loc);
	if (writeExpr)
	{
		auto writeStmt = awst::makeExpressionStatement(std::move(writeExpr), loc);
		m_ctx.pendingStatements.push_back(std::move(writeStmt));
	}

	auto vc = std::make_shared<awst::VoidConstant>();
	vc->sourceLocation = loc;
	vc->wtype = awst::WType::voidType();
	return vc;
}

std::shared_ptr<awst::Expression> SolArrayMethod::handleBoxArray(
	std::string const& _memberName,
	Expression const& _baseExpr,
	VariableDeclaration const& _varDecl)
{
	auto const* solArrType = dynamic_cast<ArrayType const*>(_varDecl.type());
	auto* rawElemType = m_ctx.typeMapper.map(solArrType->baseType());
	auto* elemType = m_ctx.typeMapper.mapSolTypeToARC4(solArrType->baseType());
	auto* arrWType = m_ctx.typeMapper.map(solArrType);

	auto const* ident = dynamic_cast<Identifier const*>(&_baseExpr);
	std::string arrayVarName = ident->name();

	// Build BoxValueExpression
	auto boxKey = awst::makeUtf8BytesConstant(arrayVarName, m_loc, awst::WType::boxKeyType());

	auto boxExpr = std::make_shared<awst::BoxValueExpression>();
	boxExpr->sourceLocation = m_loc;
	boxExpr->wtype = arrWType;
	boxExpr->key = boxKey;
	boxExpr->existsAssertionMessage = std::nullopt;

	// StateGet wrapper for reads (returns empty array if box missing)
	auto emptyArr = std::make_shared<awst::NewArray>();
	emptyArr->sourceLocation = m_loc;
	emptyArr->wtype = arrWType;

	auto stateGet = std::make_shared<awst::StateGet>();
	stateGet->sourceLocation = m_loc;
	stateGet->wtype = arrWType;
	stateGet->field = boxExpr;
	stateGet->defaultValue = emptyArr;

	std::shared_ptr<awst::Expression> writeExpr = boxExpr;

	if (_memberName == "push" && !m_call.arguments().empty())
	{
		auto val = buildExpr(*m_call.arguments()[0]);

		auto encoded = std::make_shared<awst::ARC4Encode>();
		encoded->sourceLocation = m_loc;
		encoded->wtype = elemType;
		encoded->value = std::move(val);

		auto singleArr = std::make_shared<awst::NewArray>();
		singleArr->sourceLocation = m_loc;
		singleArr->wtype = arrWType;
		singleArr->values.push_back(std::move(encoded));

		auto e = std::make_shared<awst::ArrayExtend>();
		e->sourceLocation = m_loc;
		e->wtype = awst::WType::voidType();
		e->base = writeExpr;
		e->other = std::move(singleArr);
		return e;
	}
	else if (_memberName == "push" && m_call.arguments().empty())
	{
		// push() with no args — use ArrayExtend with a zero-valued element.
		// This lets puya handle the ARC4 length header update correctly,
		// instead of manual box_resize which doesn't update the header.
		// When SolAssignment stashed a pending push value (pattern
		// `arr.push() = value`), use that as the element and return the
		// ArrayExtend directly so the assignment returns a real void
		// expression (not a VoidConstant target, which puya rejects).
		std::shared_ptr<awst::Expression> elem;
		bool fromAssign = static_cast<bool>(m_ctx.pendingArrayPushValue);
		if (fromAssign)
		{
			auto coerced = builder::TypeCoercion::coerceForAssignment(
				std::move(m_ctx.pendingArrayPushValue), rawElemType, m_loc);
			auto encoded = std::make_shared<awst::ARC4Encode>();
			encoded->sourceLocation = m_loc;
			encoded->wtype = elemType;
			encoded->value = std::move(coerced);
			elem = std::move(encoded);
		}
		else
		{
			elem = builder::TypeCoercion::makeDefaultValue(elemType, m_loc);
		}

		auto singleArr = std::make_shared<awst::NewArray>();
		singleArr->sourceLocation = m_loc;
		singleArr->wtype = arrWType;
		singleArr->values.push_back(std::move(elem));

		auto e = std::make_shared<awst::ArrayExtend>();
		e->sourceLocation = m_loc;
		e->wtype = awst::WType::voidType();
		e->base = writeExpr;
		e->other = std::move(singleArr);

		if (fromAssign)
			return e;

		auto extendStmt = awst::makeExpressionStatement(std::move(e), m_loc);
		m_ctx.pendingStatements.push_back(std::move(extendStmt));

		auto vc = std::make_shared<awst::VoidConstant>();
		vc->sourceLocation = m_loc;
		vc->wtype = awst::WType::voidType();
		return vc;
	}
	else if (_memberName == "pop")
	{
		auto popExpr = std::make_shared<awst::ArrayPop>();
		popExpr->sourceLocation = m_loc;
		popExpr->wtype = elemType;
		popExpr->base = writeExpr;

		auto decode = std::make_shared<awst::ARC4Decode>();
		decode->sourceLocation = m_loc;
		decode->wtype = rawElemType;
		decode->value = std::move(popExpr);
		return decode;
	}

	auto vc = std::make_shared<awst::VoidConstant>();
	vc->sourceLocation = m_loc;
	vc->wtype = awst::WType::voidType();
	return vc;
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
				auto itob = std::make_shared<awst::IntrinsicCall>();
				itob->sourceLocation = m_loc;
				itob->wtype = awst::WType::bytesType();
				itob->opCode = "itob";
				itob->stackArgs.push_back(std::move(byteVal));

				auto seven = awst::makeIntegerConstant("7", m_loc);
				auto one = awst::makeIntegerConstant("1", m_loc);

				auto extract = std::make_shared<awst::IntrinsicCall>();
				extract->sourceLocation = m_loc;
				extract->wtype = awst::WType::bytesType();
				extract->opCode = "extract3";
				extract->stackArgs.push_back(std::move(itob));
				extract->stackArgs.push_back(std::move(seven));
				extract->stackArgs.push_back(std::move(one));
				byteVal = std::move(extract);
			}
			else if (byteVal->wtype != awst::WType::bytesType())
			{
				byteVal = builder::TypeCoercion::stringToBytes(std::move(byteVal), m_loc);
				if (byteVal->wtype != awst::WType::bytesType())
				{
					auto cast = awst::makeReinterpretCast(std::move(byteVal), awst::WType::bytesType(), m_loc);
					byteVal = std::move(cast);
				}
			}

			auto cat = std::make_shared<awst::IntrinsicCall>();
			cat->sourceLocation = m_loc;
			cat->wtype = baseWtype;
			cat->opCode = "concat";
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
					auto encode = std::make_shared<awst::ARC4Encode>();
					encode->sourceLocation = m_loc;
					encode->wtype = elemType;
					encode->value = std::move(val);
					val = std::move(encode);
				}
			}
			auto singleArr = std::make_shared<awst::NewArray>();
			singleArr->sourceLocation = m_loc;
			singleArr->wtype = baseWtype;
			singleArr->values.push_back(std::move(val));

			auto e = std::make_shared<awst::ArrayExtend>();
			e->sourceLocation = m_loc;
			e->wtype = awst::WType::voidType();
			e->base = std::move(base);
			e->other = std::move(singleArr);
			return e;
		}
	}
	else if (_memberName == "pop")
	{
		auto e = std::make_shared<awst::ArrayPop>();
		e->sourceLocation = m_loc;
		e->wtype = awst::WType::voidType();
		e->base = std::move(base);
		return e;
	}

	auto vc = std::make_shared<awst::VoidConstant>();
	vc->sourceLocation = m_loc;
	vc->wtype = awst::WType::voidType();
	return vc;
}

} // namespace puyasol::builder::sol_ast
