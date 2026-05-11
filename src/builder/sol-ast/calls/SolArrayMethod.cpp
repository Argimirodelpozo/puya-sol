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

	// Mapping entry (or similar indexed access) with dynamic array value:
	// `m[k].push()`, `m[k].push(v)`, `m[k].pop()`. The base IndexAccess lowers
	// to a BoxValueExpression (wrapped in StateGet when read). Unwrap and emit
	// ArrayExtend/ArrayPop on the raw BoxValueExpression so puya's ARC4 dynamic
	// array codegen handles the length header + element append in box storage.
	if (auto const* innerIA = dynamic_cast<IndexAccess const*>(&baseExpr))
	{
		auto const* innerArrType = dynamic_cast<ArrayType const*>(
			innerIA->annotation().type);
		if (innerArrType && innerArrType->isDynamicallySized()
			&& !innerArrType->isByteArrayOrString()
			&& (memberName == "push" || memberName == "pop"))
		{
			auto baseAwst = buildExpr(baseExpr);
			if (auto const* sg = dynamic_cast<awst::StateGet const*>(baseAwst.get()))
				baseAwst = sg->field;

			if (dynamic_cast<awst::BoxValueExpression const*>(baseAwst.get()))
			{
				auto* rawElemType = m_ctx.typeMapper.map(innerArrType->baseType());
				auto* elemType = m_ctx.typeMapper.mapSolTypeToARC4(
					innerArrType->baseType());
				auto* arrWType = baseAwst->wtype
					? baseAwst->wtype : m_ctx.typeMapper.map(innerArrType);

				// Ensure the per-entry box exists with an empty ARC4
				// dynamic-array header (`0x0000`) before ArrayExtend/ArrayPop
				// reads it. Guarded by `box_len.exists` so subsequent pushes
				// (which grew the box past 2 bytes) skip the create.
				auto emitEnsureBox = [&]() {
					auto const* bv = static_cast<awst::BoxValueExpression const*>(baseAwst.get());
					if (!bv->key)
						return;
					auto boxKey = bv->key;

					auto* tupleType = m_ctx.typeMapper.createType<awst::WTuple>(
						std::vector<awst::WType const*>{
							awst::WType::uint64Type(), awst::WType::boolType()});
					auto boxLen = awst::makeIntrinsicCall("box_len", tupleType, m_loc);
					boxLen->stackArgs.push_back(boxKey);

					auto existsVal = awst::makeTupleItem(std::move(boxLen), 1, awst::WType::boolType(), m_loc);

					auto notExists = awst::makeNot(std::move(existsVal), m_loc);

					auto createCall = awst::makeIntrinsicCall(
						"box_create", awst::WType::boolType(), m_loc);
					createCall->stackArgs.push_back(boxKey);
					createCall->stackArgs.push_back(awst::makeIntegerConstant("2", m_loc));
					auto createStmt = awst::makeExpressionStatement(
						std::move(createCall), m_loc);

					auto ifBranch = awst::makeBlock(m_loc);
					ifBranch->body.push_back(std::move(createStmt));

					m_ctx.prePendingStatements.push_back(awst::makeIfElse(
						std::move(notExists), std::move(ifBranch), nullptr, m_loc));
				};

				if (memberName == "push" && !m_call.arguments().empty())
				{
					emitEnsureBox();
					auto val = buildExpr(*m_call.arguments()[0]);
					auto encoded = awst::makeARC4Encode(std::move(val), elemType, m_loc);

					auto singleArr = awst::makeNewArray(arrWType, m_loc);
					singleArr->values.push_back(std::move(encoded));

					auto e = std::make_shared<awst::ArrayExtend>();
					e->sourceLocation = m_loc;
					e->wtype = awst::WType::voidType();
					e->base = baseAwst;
					e->other = std::move(singleArr);
					return e;
				}
				if (memberName == "push" && m_call.arguments().empty())
				{
					emitEnsureBox();
					std::shared_ptr<awst::Expression> elem;
					bool fromAssign = static_cast<bool>(m_ctx.pendingArrayPushValue);
					if (fromAssign)
					{
						auto coerced = builder::TypeCoercion::coerceForAssignment(
							std::move(m_ctx.pendingArrayPushValue), rawElemType, m_loc);
						auto encoded = awst::makeARC4Encode(std::move(coerced), elemType, m_loc);
						elem = std::move(encoded);
					}
					else
						elem = builder::TypeCoercion::makeDefaultValue(elemType, m_loc);

					auto singleArr = awst::makeNewArray(arrWType, m_loc);
					singleArr->values.push_back(std::move(elem));

					auto e = std::make_shared<awst::ArrayExtend>();
					e->sourceLocation = m_loc;
					e->wtype = awst::WType::voidType();
					e->base = baseAwst;
					e->other = std::move(singleArr);

					if (fromAssign)
						return e;

					auto stmt = awst::makeExpressionStatement(std::move(e), m_loc);
					// Use prePendingStatements so the extend runs BEFORE the
					// enclosing statement. For `arr.push().field = v` the
					// field write reads ArrayLength - 1 post-extend.
					m_ctx.prePendingStatements.push_back(std::move(stmt));

					// Solidity `arr.push()` returns a reference to the new
					// element (so `arr.push().field = v` works). Lower it as
					// IndexExpression(arr, ArrayLength(arr) - 1) evaluated
					// after the extend statement above.
					auto lenNode = awst::makeArrayLength(baseAwst, awst::WType::uint64Type(), m_loc);

					auto lastIndex = awst::makeUInt64BinOp(
						std::move(lenNode),
						awst::UInt64BinaryOperator::Sub,
						awst::makeIntegerConstant("1", m_loc),
						m_loc);

					auto idxExpr = awst::makeIndexExpression(baseAwst, std::move(lastIndex), elemType, m_loc);
					return idxExpr;
				}
				if (memberName == "pop")
				{
					auto popExpr = std::make_shared<awst::ArrayPop>();
					popExpr->sourceLocation = m_loc;
					popExpr->wtype = elemType;
					popExpr->base = baseAwst;

					auto decode = awst::makeARC4Decode(std::move(popExpr), rawElemType, m_loc);
					return decode;
				}
			}
		}
	}

	// Storage-pointer alias: `uint[] storage ptr = stateArr; ptr.push(x);`
	// The Identifier `ptr` refers to a local whose AWST alias is
	// StateGet(BoxValueExpression). ArrayExtend/ArrayPop require a writable
	// target — StateGet is read-only, so emit the op against the unwrapped
	// BoxValueExpression directly (same pattern used by SolIndexAccess).
	if (auto const* ident = dynamic_cast<Identifier const*>(&baseExpr))
	{
		if (auto const* decl = dynamic_cast<VariableDeclaration const*>(
				ident->annotation().referencedDeclaration))
		{
			if (!decl->isStateVariable())
			{
				auto aliasShared = m_scope.findStorageAlias(decl->id());
				if (aliasShared
					&& (memberName == "push" || memberName == "pop"))
				{
					auto const* solArrType = dynamic_cast<ArrayType const*>(decl->type());
					if (solArrType && !solArrType->isByteArrayOrString())
					{
						std::shared_ptr<awst::Expression> aliasExpr = aliasShared;
						// Unwrap StateGet to underlying writable target.
						if (auto const* sg = dynamic_cast<awst::StateGet const*>(
								aliasExpr.get()))
							aliasExpr = sg->field;
						// Only proceed if underlying target is a BoxValueExpression
						// (the typical case for dynamic arrays — mapped to box storage).
						if (dynamic_cast<awst::BoxValueExpression const*>(aliasExpr.get()))
						{
							auto* rawElemType = m_ctx.typeMapper.map(solArrType->baseType());
							auto* elemType = m_ctx.typeMapper.mapSolTypeToARC4(solArrType->baseType());
							auto* arrWType = aliasExpr->wtype
								? aliasExpr->wtype
								: m_ctx.typeMapper.map(solArrType);

							if (memberName == "push" && !m_call.arguments().empty())
							{
								auto val = buildExpr(*m_call.arguments()[0]);
								auto encoded = awst::makeARC4Encode(std::move(val), elemType, m_loc);

								auto singleArr = awst::makeNewArray(arrWType, m_loc);
								singleArr->values.push_back(std::move(encoded));

								auto e = std::make_shared<awst::ArrayExtend>();
								e->sourceLocation = m_loc;
								e->wtype = awst::WType::voidType();
								e->base = aliasExpr;
								e->other = std::move(singleArr);
								return e;
							}
							if (memberName == "push" && m_call.arguments().empty())
							{
								std::shared_ptr<awst::Expression> elem;
								bool fromAssign = static_cast<bool>(m_ctx.pendingArrayPushValue);
								if (fromAssign)
								{
									auto coerced = builder::TypeCoercion::coerceForAssignment(
										std::move(m_ctx.pendingArrayPushValue), rawElemType, m_loc);
									auto encoded = awst::makeARC4Encode(std::move(coerced), elemType, m_loc);
									elem = std::move(encoded);
								}
								else
									elem = builder::TypeCoercion::makeDefaultValue(elemType, m_loc);

								auto singleArr = awst::makeNewArray(arrWType, m_loc);
								singleArr->values.push_back(std::move(elem));

								auto e = std::make_shared<awst::ArrayExtend>();
								e->sourceLocation = m_loc;
								e->wtype = awst::WType::voidType();
								e->base = aliasExpr;
								e->other = std::move(singleArr);

								if (fromAssign)
									return e;

								auto stmt = awst::makeExpressionStatement(std::move(e), m_loc);
								m_ctx.pendingStatements.push_back(std::move(stmt));
								auto vc = awst::makeVoidConstant(m_loc);
								return vc;
							}
							if (memberName == "pop")
							{
								auto popExpr = std::make_shared<awst::ArrayPop>();
								popExpr->sourceLocation = m_loc;
								popExpr->wtype = elemType;
								popExpr->base = aliasExpr;

								auto decode = awst::makeARC4Decode(std::move(popExpr), rawElemType, m_loc);
								return decode;
							}
						}
					}
				}
			}
		}
	}

	// Check if this is a state variable array
	// `bytes(stringStateVar).push(...)` / `.pop()` — Solidity allows calling
	// array methods on the bytes view of a string state variable, and the
	// result modifies the underlying state. The base AST shape here is
	// `FunctionCall(TypeConversion, [Identifier])`, not a bare Identifier,
	// so the standard state-var paths below don't fire and the call falls
	// through to a default route that produces broken codegen (treats it
	// as `x = x + 1`). Detect this shape and unwrap to the underlying
	// Identifier so the bytes/string state-var .push/.pop branches handle it.
	Expression const* effectiveBase = &baseExpr;
	if (auto const* castCall = dynamic_cast<FunctionCall const*>(&baseExpr))
	{
		if (*castCall->annotation().kind == FunctionCallKind::TypeConversion
			&& castCall->arguments().size() == 1)
		{
			auto const* convArg = castCall->arguments()[0].get();
			if (auto const* convIdent = dynamic_cast<Identifier const*>(convArg))
			{
				auto const* convDecl = dynamic_cast<VariableDeclaration const*>(
					convIdent->annotation().referencedDeclaration);
				if (convDecl && convDecl->isStateVariable())
				{
					auto const* convType = dynamic_cast<ArrayType const*>(convDecl->type());
					if (convType && convType->isByteArrayOrString())
						effectiveBase = convIdent;
				}
			}
		}
	}

	if (auto const* ident = dynamic_cast<Identifier const*>(effectiveBase))
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
					auto lenCall = awst::makeLen(readVal, loc);

					auto one = awst::makeIntegerConstant("1", loc);
					auto newLen = awst::makeUInt64BinOp(std::move(lenCall), awst::UInt64BinaryOperator::Sub, std::move(one), loc);

					// extract3(readVal, 0, len-1)
					auto zero = awst::makeIntegerConstant("0", loc);

					auto extract = awst::makeExtract3(readVal, std::move(zero), std::move(newLen), loc);
					if (kind == awst::AppStorageKind::Box)
					{
						// Box: store shrunk in temp, box_del, box_put
						static int popTmpCounter = 0;
						std::string tmpName = "__bytes_pop_tmp_" + std::to_string(popTmpCounter++);

						auto tmpTarget = awst::makeVarExpression(tmpName, awst::WType::bytesType(), loc);
						auto assignTmp = awst::makeAssignmentStatement(tmpTarget, std::move(extract), loc);
						m_ctx.pendingStatements.push_back(std::move(assignTmp));

						auto del = awst::makeIntrinsicCall("box_del", awst::WType::boolType(), loc);
						del->stackArgs.push_back(awst::makeUtf8BytesConstant(varName, loc));
						auto delStmt = awst::makeExpressionStatement(std::move(del), loc);
						m_ctx.pendingStatements.push_back(std::move(delStmt));

						auto tmpRead = awst::makeVarExpression(tmpName, awst::WType::bytesType(), loc);
						auto put = awst::makeIntrinsicCall("box_put", awst::WType::voidType(), loc);
						put->stackArgs.push_back(awst::makeUtf8BytesConstant(varName, loc));
						put->stackArgs.push_back(std::move(tmpRead));
						auto putStmt = awst::makeExpressionStatement(std::move(put), loc);
						m_ctx.pendingStatements.push_back(std::move(putStmt));
					}
					else
					{
						auto put = awst::makeIntrinsicCall("app_global_put", awst::WType::voidType(), loc);
						put->stackArgs.push_back(awst::makeUtf8BytesConstant(varName, loc));
						put->stackArgs.push_back(std::move(extract));
						auto stmt = awst::makeExpressionStatement(std::move(put), loc);
						m_ctx.pendingStatements.push_back(std::move(stmt));
					}

					auto vc = awst::makeVoidConstant(loc);
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

					// Build the push value. `bytes.push(b)` takes a `bytes1`
					// arg; Solidity implicitly converts uint8 / int literals
					// to bytes1. Our buildExpr returns a uint64 for those, so
					// itob+extract the last byte. For string types we use the
					// existing stringToBytes path. For bytes-typed args (rare
					// — would only arise from `bytes(x).push(b)` where b is
					// already bytes), pass through.
					std::shared_ptr<awst::Expression> pushVal;
					if (!m_call.arguments().empty())
					{
						pushVal = buildExpr(*m_call.arguments()[0]);
						auto* pvT = pushVal ? pushVal->wtype : nullptr;
						if (pvT == awst::WType::uint64Type())
						{
							// uint64 → 1-byte bytes via itob (8 bytes BE) + extract last.
							auto itob = awst::makeIntrinsicCall(
								"itob", awst::WType::bytesType(), loc);
							itob->stackArgs.push_back(std::move(pushVal));
							auto extr = awst::makeIntrinsicCall(
								"extract3", awst::WType::bytesType(), loc);
							extr->stackArgs.push_back(std::move(itob));
							extr->stackArgs.push_back(awst::makeIntegerConstant("7", loc));
							extr->stackArgs.push_back(awst::makeIntegerConstant("1", loc));
							pushVal = std::move(extr);
						}
						else
						{
							pushVal = builder::TypeCoercion::stringToBytes(std::move(pushVal), loc);
						}
					}
					else
					{
						pushVal = awst::makeBytesConstant({0}, loc);
					}

					// concat(current, pushVal)
					auto cat = awst::makeIntrinsicCall("concat", awst::WType::bytesType(), loc);
					cat->stackArgs.push_back(std::move(readVal));
					cat->stackArgs.push_back(std::move(pushVal));

					if (kind == awst::AppStorageKind::Box)
					{
						// Box: store concat in temp, box_del, box_put(key, temp)
						// box_put requires exact size match, so we delete+recreate
						static int tmpCounter = 0;
						std::string tmpName = "__bytes_push_tmp_" + std::to_string(tmpCounter++);

						auto tmpTarget = awst::makeVarExpression(tmpName, awst::WType::bytesType(), loc);

						auto assignTmp = awst::makeAssignmentStatement(tmpTarget, std::move(cat), loc);
						m_ctx.pendingStatements.push_back(std::move(assignTmp));

						auto del = awst::makeIntrinsicCall("box_del", awst::WType::boolType(), loc);
						del->stackArgs.push_back(awst::makeUtf8BytesConstant(varName, loc));
						auto delStmt = awst::makeExpressionStatement(std::move(del), loc);
						m_ctx.pendingStatements.push_back(std::move(delStmt));

						auto tmpRead = awst::makeVarExpression(tmpName, awst::WType::bytesType(), loc);
						auto put = awst::makeIntrinsicCall("box_put", awst::WType::voidType(), loc);
						put->stackArgs.push_back(awst::makeUtf8BytesConstant(varName, loc));
						put->stackArgs.push_back(std::move(tmpRead));
						auto putStmt = awst::makeExpressionStatement(std::move(put), loc);
						m_ctx.pendingStatements.push_back(std::move(putStmt));
					}
					else
					{
						auto put = awst::makeIntrinsicCall("app_global_put", awst::WType::voidType(), loc);
						put->stackArgs.push_back(awst::makeUtf8BytesConstant(varName, loc));
						put->stackArgs.push_back(std::move(cat));
						auto stmt = awst::makeExpressionStatement(std::move(put), loc);
						m_ctx.pendingStatements.push_back(std::move(stmt));
					}

					auto vc = awst::makeVoidConstant(loc);
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
	auto tmpAssign = awst::makeAssignmentStatement(tmpTarget, std::move(structRead), loc);
	m_ctx.pendingStatements.push_back(std::move(tmpAssign));

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
				{
					auto encode = awst::makeARC4Encode(std::move(val), elemType, loc);
					val = std::move(encode);
				}
			}
		}
		else
		{
			val = builder::TypeCoercion::makeDefaultValue(elemType, loc);
		}

		auto singleArr = awst::makeNewArray(rawFieldType, loc);
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

	auto vc = awst::makeVoidConstant(loc);
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

	// Mapping-element arrays — `mapping(K=>V)[] a` and friends — store no
	// element bytes inline. Each `a[i][k]` is its own box derived from
	// `a`, `i`, and `sha256(k)` (handled in SolIndexAccess). The array
	// box itself only tracks length: 2-byte big-endian header, no
	// element tail. This is also what gives us EVM's "delete leaves
	// data at hash" semantic for free — `delete a; a.push();` reuses the
	// same `(i, k)` box keys, and the previously-written values persist
	// because we never touched those boxes.
	bool elemIsMapping = dynamic_cast<MappingType const*>(solArrType->baseType()) != nullptr;
	if (elemIsMapping && (_memberName == "push" || _memberName == "pop"))
		return handleMappingElementArrayLengthOp(_memberName, _varDecl, arrayVarName);

	// Build BoxValueExpression
	auto boxKey = awst::makeUtf8BytesConstant(arrayVarName, m_loc, awst::WType::boxKeyType());

	auto boxExpr = awst::makeBoxValueExpression(boxKey, arrWType, m_loc);

	// StateGet wrapper for reads (returns empty array if box missing)
	auto emptyArr = awst::makeNewArray(arrWType, m_loc);

	auto stateGet = awst::makeStateGet(boxExpr, emptyArr, arrWType, m_loc);

	std::shared_ptr<awst::Expression> writeExpr = boxExpr;

	if (_memberName == "push" && !m_call.arguments().empty())
	{
		auto val = buildExpr(*m_call.arguments()[0]);

		auto encoded = awst::makeARC4Encode(std::move(val), elemType, m_loc);

		auto singleArr = awst::makeNewArray(arrWType, m_loc);
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
			auto encoded = awst::makeARC4Encode(std::move(coerced), elemType, m_loc);
			elem = std::move(encoded);
		}
		else
		{
			elem = builder::TypeCoercion::makeDefaultValue(elemType, m_loc);
		}

		auto singleArr = awst::makeNewArray(arrWType, m_loc);
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

		auto vc = awst::makeVoidConstant(m_loc);
		return vc;
	}
	else if (_memberName == "pop")
	{
		auto popExpr = std::make_shared<awst::ArrayPop>();
		popExpr->sourceLocation = m_loc;
		popExpr->wtype = elemType;
		popExpr->base = writeExpr;

		auto decode = awst::makeARC4Decode(std::move(popExpr), rawElemType, m_loc);
		return decode;
	}

	auto vc = awst::makeVoidConstant(m_loc);
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
				auto itob = awst::makeItob(std::move(byteVal), m_loc);

				auto seven = awst::makeIntegerConstant("7", m_loc);
				auto one = awst::makeIntegerConstant("1", m_loc);

				auto extract = awst::makeExtract3(std::move(itob), std::move(seven), std::move(one), m_loc);
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
			auto singleArr = awst::makeNewArray(baseWtype, m_loc);
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

	auto vc = awst::makeVoidConstant(m_loc);
	return vc;
}

std::shared_ptr<awst::Expression> SolArrayMethod::handleMappingElementArrayLengthOp(
	std::string const& _memberName,
	solidity::frontend::VariableDeclaration const& /*_varDecl*/,
	std::string const& _arrayVarName)
{
	// Box layout: 2-byte big-endian length, no element data.
	// Read current length:  box_get → bytes; if empty (deleted/not-yet-created)
	//                       len=0; else extract_uint16(0).
	// Write new length:     itob(new_len) → 8 bytes → extract last 2 → box_put.

	auto boxKey = awst::makeUtf8BytesConstant(
		_arrayVarName, m_loc, awst::WType::boxKeyType());

	// Read box bytes (or empty if missing).
	auto boxRead = [&]() -> std::shared_ptr<awst::Expression> {
		auto box = awst::makeBoxValueExpression(boxKey, awst::WType::bytesType(), m_loc);
		return awst::makeStateGet(box, awst::makeBytesConstant({}, m_loc), awst::WType::bytesType(), m_loc);
	};

	// uint64 currentLen = (box_bytes.length() >= 2) ? extract_uint16(box, 0) : 0
	// We just call extract_uint16 if length>=2; the only time length<2 is
	// after a fresh `delete a` or never-written. Since we always write at
	// least 2 bytes when the box exists, "exists implies len>=2" — guard
	// with a `len(box) > 0` ternary to avoid extract_uint16 on an empty.
	auto bytes = boxRead();
	auto lenOfBytes = awst::makeLen(bytes, m_loc);
	auto isNonEmpty = awst::makeNumericCompare(
		std::move(lenOfBytes),
		awst::NumericComparison::Gt,
		awst::makeIntegerConstant("0", m_loc),
		m_loc);
	auto extractLen = awst::makeIntrinsicCall("extract_uint16", awst::WType::uint64Type(), m_loc);
	extractLen->stackArgs.push_back(boxRead());
	extractLen->stackArgs.push_back(awst::makeIntegerConstant("0", m_loc));
	auto cur = awst::makeConditional(
		std::move(isNonEmpty), std::move(extractLen),
		awst::makeIntegerConstant("0", m_loc),
		awst::WType::uint64Type(), m_loc);

	// new_len = cur ± 1
	auto delta = awst::makeIntegerConstant("1", m_loc);
	auto newLen = awst::makeUInt64BinOp(
		std::move(cur),
		_memberName == "push" ? awst::UInt64BinaryOperator::Add
							  : awst::UInt64BinaryOperator::Sub,
		std::move(delta), m_loc);

	// 2-byte big-endian = extract3(itob(new_len), 6, 2)
	auto itob = awst::makeItob(std::move(newLen), m_loc);
	auto extract = awst::makeExtract3(std::move(itob), awst::makeIntegerConstant("6", m_loc), awst::makeIntegerConstant("2", m_loc), m_loc);
	// box_put(arrayVarName, len_bytes)
	auto put = awst::makeIntrinsicCall("box_put", awst::WType::voidType(), m_loc);
	put->stackArgs.push_back(boxKey);
	put->stackArgs.push_back(std::move(extract));

	auto stmt = awst::makeExpressionStatement(std::move(put), m_loc);
	m_ctx.pendingStatements.push_back(std::move(stmt));

	auto vc = awst::makeVoidConstant(m_loc);
	return vc;
}

} // namespace puyasol::builder::sol_ast
