/// @file SolArrayMethod.cpp
/// array.push(val), array.push(), and array.pop().
/// Box-backed arrays read/write from box storage; memory arrays use AWST nodes directly.

#include "builder/sol-ast/calls/SolArrayMethod.h"
#include "awst/NameGen.h"
#include "builder/storage/StorageMapper.h"
#include "builder/sol-types/TypeMapper.h"
#include "builder/sol-types/TypeCoercion.h"

#include <libsolidity/ast/AST.h>

#include <functional>

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

	// `m[k].push()/.pop()`: IndexAccess base lowers to BoxValueExpression (wrapped
	// in StateGet when read). Unwrap and emit ArrayExtend/ArrayPop on the raw
	// BoxValueExpression so puya's ARC4 dyn-array codegen handles box storage.
	if (auto const* innerIA = dynamic_cast<IndexAccess const*>(&baseExpr))
	{
		auto const* innerArrType = dynamic_cast<ArrayType const*>(
			innerIA->annotation().type);
		if (innerArrType && innerArrType->isDynamicallySized()
			&& !innerArrType->isByteArrayOrString()
			&& (memberName == "push" || memberName == "pop"))
		{
			auto baseAwst = buildExpr(baseExpr);
			// Unwrap StateGet through the chain to the writable BoxValueExpression.
			baseAwst = awst::makeWritableTarget(baseAwst);

			if (dynamic_cast<awst::BoxValueExpression const*>(baseAwst.get())
				|| dynamic_cast<awst::IndexExpression const*>(baseAwst.get())
				|| dynamic_cast<awst::FieldExpression const*>(baseAwst.get()))
			{
				auto* rawElemType = m_ctx.typeMapper.map(innerArrType->baseType());
				auto* elemType = m_ctx.typeMapper.mapSolTypeToARC4(
					innerArrType->baseType());
				auto* arrWType = baseAwst->wtype
					? baseAwst->wtype : m_ctx.typeMapper.map(innerArrType);

				// Ensure the per-entry box has the empty ARC4 dyn-array header
				// (0x0000) before ArrayExtend/ArrayPop. Guarded by box_len.exists
				// so subsequent pushes (box already >2 bytes) skip the create.
				auto emitEnsureBox = [&]() {
					// Centralized box-lifecycle: a push/pop RESIZE needs the root box (bare dyn-array box, or the
					// STRUCT box for `m[k].arr.push()` reached through a FieldExpression) to exist first. Shared
					// with maybePrePopulateBox / SolAssignmentStructField via makeEnsureRootBoxForWrite.
					if (auto stmt = builder::StorageMapper::makeEnsureRootBoxForWrite(
							m_ctx.typeMapper, baseAwst, /*isResize=*/true, m_loc))
						m_ctx.queuePrePending(std::move(stmt));
				};

				if (memberName == "push" && !m_call.arguments().empty())
				{
					emitEnsureBox();
					auto val = buildExpr(*m_call.arguments()[0]);
					auto encoded = awst::makeARC4Encode(std::move(val), elemType, m_loc);
					return awst::makeArrayPushOne(baseAwst, std::move(encoded), arrWType, m_loc);
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
						elem = awst::makeARC4Encode(std::move(coerced), elemType, m_loc);
					}
					else
						elem = builder::TypeCoercion::makeDefaultValue(elemType, m_loc);

					auto e = awst::makeArrayPushOne(baseAwst, std::move(elem), arrWType, m_loc);

					if (fromAssign)
						return e;

					// queuePreStmt: extend runs before the enclosing statement.
					// `arr.push().field = v` reads ArrayLength-1 post-extend.
					m_ctx.queuePreStmt(std::move(e), m_loc);

					// `arr.push()` returns a ref to the new element as
					// IndexExpression(arr, ArrayLength(arr)-1).
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
					return awst::makeArrayPopDecode(baseAwst, elemType, rawElemType, m_loc);
			}
		}
	}

	// Storage-pointer alias: .
	// StateGet is read-only; unwrap to the BoxValueExpression for ArrayExtend/ArrayPop.
	if (auto const* ident = dynamic_cast<Identifier const*>(&baseExpr))
	{
		if (auto const* decl = dynamic_cast<VariableDeclaration const*>(
				ident->annotation().referencedDeclaration))
		{
			if (!decl->isStateVariable())
			{
				auto const* alias = m_scope.findStorageAlias(decl->id());
				if (alias
					&& (memberName == "push" || memberName == "pop"))
				{
					auto const* solArrType = dynamic_cast<ArrayType const*>(decl->type());
					// bytes/string storage alias (ternary-init pointer): concat
					// push / shrink pop against the aliased BOX (runtime key) —
					// the state-var twin below is name-keyed and never fires for
					// locals.
					if (solArrType && solArrType->isByteArrayOrString())
					{
						auto unwrapped = awst::unwrapStateGet(alias->expr);
						auto const* bv = dynamic_cast<awst::BoxValueExpression const*>(unwrapped.get());
						if (bv && bv->key)
						{
							auto loc = m_loc;
							auto readVal = alias->expr; // StateGetWithDefault read
							std::string tmpName = "__bytes_alias_tmp_"
								+ std::to_string(awst::NameGen::next("SolArrayMethod.tmpCounter"));
							auto tmpTarget = awst::makeVarExpression(
								tmpName, awst::WType::bytesType(), loc);
							if (memberName == "push")
							{
								std::shared_ptr<awst::Expression> pushVal;
								if (!m_call.arguments().empty())
								{
									pushVal = buildExpr(*m_call.arguments()[0]);
									if (pushVal && pushVal->wtype == awst::WType::uint64Type())
									{
										auto itob = awst::makeIntrinsicCall(
											"itob", awst::WType::bytesType(), loc);
										itob->stackArgs.push_back(std::move(pushVal));
										auto extr = awst::makeIntrinsicCall(
											"extract3", awst::WType::bytesType(), loc);
										extr->stackArgs.push_back(std::move(itob));
										extr->stackArgs.push_back(awst::makeIntegerConstant("7", loc));
										extr->stackArgs.push_back(awst::makeOne(loc));
										pushVal = std::move(extr);
									}
									else
										pushVal = builder::TypeCoercion::stringToBytes(
											std::move(pushVal), loc);
								}
								else
									pushVal = awst::makeBytesConstant({0}, loc);
								m_ctx.queuePending(awst::makeAssignmentStatement(tmpTarget,
									awst::makeConcat(std::move(readVal), std::move(pushVal), loc),
									loc));
							}
							else
							{
								auto newLen = awst::makeUInt64BinOp(
									awst::makeLen(readVal, loc),
									awst::UInt64BinaryOperator::Sub, awst::makeOne(loc), loc);
								m_ctx.queuePending(awst::makeAssignmentStatement(tmpTarget,
									awst::makeExtract3(readVal, awst::makeZero(loc),
										std::move(newLen), loc),
									loc));
							}
							m_ctx.queueStmt(awst::makeBoxDel(bv->key, loc), loc);
							m_ctx.queueStmt(awst::makeBoxPut(bv->key,
								awst::makeVarExpression(tmpName, awst::WType::bytesType(), loc),
								loc), loc);
							return awst::makeVoidConstant(loc);
						}
					}
					if (solArrType && !solArrType->isByteArrayOrString())
					{
						std::shared_ptr<awst::Expression> aliasExpr = alias->expr;
						// Unwrap StateGet (same transform as m[k].push() path above).
						// Writable targets: BoxValueExpression / IndexExpression / FieldExpression.
						aliasExpr = awst::unwrapStateGet(std::move(aliasExpr));
						aliasExpr = awst::makeWritableTarget(aliasExpr);
						if (dynamic_cast<awst::BoxValueExpression const*>(aliasExpr.get())
							|| dynamic_cast<awst::IndexExpression const*>(aliasExpr.get())
							|| dynamic_cast<awst::FieldExpression const*>(aliasExpr.get()))
						{
							auto* rawElemType = m_ctx.typeMapper.map(solArrType->baseType());
							auto* elemType = m_ctx.typeMapper.mapSolTypeToARC4(solArrType->baseType());
							auto* arrWType = aliasExpr->wtype
								? aliasExpr->wtype
								: m_ctx.typeMapper.map(solArrType);

							// Same as emitEnsureBox above: ensure the aliased box has
							// the 0x0000 header before ArrayExtend/ArrayPop. Without
							// this, `A(state[k])` + push trips the box-exists assert.
							auto emitEnsureAliasBox = [&]() {
								auto const* bv = dynamic_cast<awst::BoxValueExpression const*>(aliasExpr.get());
								if (!bv || !bv->key)
									return;
								auto boxKey = bv->key;
								auto boxLen = builder::StorageMapper::makeBoxLenTuple(
									m_ctx.typeMapper, boxKey, m_loc);
								auto existsVal = awst::makeTupleItem(std::move(boxLen), 1, awst::WType::boolType(), m_loc);
								auto notExists = awst::makeNot(std::move(existsVal), m_loc);
								auto createCall = awst::makeBoxCreate(
									boxKey, awst::makeIntegerConstant("2", m_loc), m_loc);
								auto createStmt = awst::makeExpressionStatement(
									std::move(createCall), m_loc);
								auto ifBranch = awst::makeBlock(m_loc);
								ifBranch->body.push_back(std::move(createStmt));
								m_ctx.queuePrePending(awst::makeIfElse(
									std::move(notExists), std::move(ifBranch), nullptr, m_loc));
							};

							if (memberName == "push" && !m_call.arguments().empty())
							{
								emitEnsureAliasBox();
								auto val = buildExpr(*m_call.arguments()[0]);
								auto encoded = awst::makeARC4Encode(std::move(val), elemType, m_loc);
								return awst::makeArrayPushOne(aliasExpr, std::move(encoded), arrWType, m_loc);
							}
							if (memberName == "push" && m_call.arguments().empty())
							{
								emitEnsureAliasBox();
								std::shared_ptr<awst::Expression> elem;
								bool fromAssign = static_cast<bool>(m_ctx.pendingArrayPushValue);
								if (fromAssign)
								{
									auto coerced = builder::TypeCoercion::coerceForAssignment(
										std::move(m_ctx.pendingArrayPushValue), rawElemType, m_loc);
									elem = awst::makeARC4Encode(std::move(coerced), elemType, m_loc);
								}
								else
									elem = builder::TypeCoercion::makeDefaultValue(elemType, m_loc);

								auto e = awst::makeArrayPushOne(aliasExpr, std::move(elem), arrWType, m_loc);

								if (fromAssign)
									return e;

								m_ctx.queueStmt(std::move(e), m_loc);
								return awst::makeVoidConstant(m_loc);
							}
							if (memberName == "pop")
							{
								emitEnsureAliasBox();
								return awst::makeArrayPopDecode(aliasExpr, elemType, rawElemType, m_loc);
							}
						}
					}
				}
			}
		}
	}

	// `bytes(stringStateVar).push(...)/.pop()`: the base AST is
	// FunctionCall(TypeConversion,[Identifier]), not a bare Identifier,
	// so the state-var paths below don't fire. Unwrap to the Identifier.
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

					auto one = awst::makeOne(loc);
					auto newLen = awst::makeUInt64BinOp(std::move(lenCall), awst::UInt64BinaryOperator::Sub, std::move(one), loc);

					// extract3(readVal, 0, len-1)
					auto zero = awst::makeZero(loc);

					auto extract = awst::makeExtract3(readVal, std::move(zero), std::move(newLen), loc);
					if (kind == awst::AppStorageKind::Box)
					{
						// Box: shrunk→temp, box_del, box_put.
						std::string tmpName = "__bytes_pop_tmp_" + std::to_string(awst::NameGen::next("SolArrayMethod.popTmpCounter"));

						auto tmpTarget = awst::makeVarExpression(tmpName, awst::WType::bytesType(), loc);
						m_ctx.queuePending(awst::makeAssignmentStatement(tmpTarget, std::move(extract), loc));

						m_ctx.queueStmt(awst::makeBoxDel(awst::makeUtf8BytesConstant(varName, loc), loc), loc);

						auto tmpRead = awst::makeVarExpression(tmpName, awst::WType::bytesType(), loc);
						m_ctx.queueStmt(awst::makeBoxPut(
							awst::makeUtf8BytesConstant(varName, loc),
							std::move(tmpRead), loc), loc);
					}
					else
					{
						m_ctx.queueStmt(awst::makeAppGlobalPut(
							awst::makeUtf8BytesConstant(varName, loc),
							std::move(extract), loc), loc);
					}

					return awst::makeVoidConstant(loc);
				}
			}

			// bytes/string state var push: concat-based, not element-by-element.
			// Must come BEFORE the generic box array handler.
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

					// `bytes.push(b)`: takes bytes1. uint8/int literals arrive
					// as uint64 — itob+extract last byte. String→stringToBytes.
					std::shared_ptr<awst::Expression> pushVal;
					if (!m_call.arguments().empty())
					{
						pushVal = buildExpr(*m_call.arguments()[0]);
						auto* pvT = pushVal ? pushVal->wtype : nullptr;
						if (pvT == awst::WType::uint64Type())
						{
							// uint64 → 1-byte bytes: itob (8 bytes BE) + extract last.
							auto itob = awst::makeIntrinsicCall(
								"itob", awst::WType::bytesType(), loc);
							itob->stackArgs.push_back(std::move(pushVal));
							auto extr = awst::makeIntrinsicCall(
								"extract3", awst::WType::bytesType(), loc);
							extr->stackArgs.push_back(std::move(itob));
							extr->stackArgs.push_back(awst::makeIntegerConstant("7", loc));
							extr->stackArgs.push_back(awst::makeOne(loc));
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
					auto cat = awst::makeConcat(std::move(readVal), std::move(pushVal), loc);

					if (kind == awst::AppStorageKind::Box)
					{
						// Box: concat→temp, box_del, box_put (exact-size match required).
						std::string tmpName = "__bytes_push_tmp_" + std::to_string(awst::NameGen::next("SolArrayMethod.tmpCounter"));

						auto tmpTarget = awst::makeVarExpression(tmpName, awst::WType::bytesType(), loc);
						m_ctx.queuePending(awst::makeAssignmentStatement(tmpTarget, std::move(cat), loc));

						m_ctx.queueStmt(awst::makeBoxDel(awst::makeUtf8BytesConstant(varName, loc), loc), loc);

						auto tmpRead = awst::makeVarExpression(tmpName, awst::WType::bytesType(), loc);
						m_ctx.queueStmt(awst::makeBoxPut(
							awst::makeUtf8BytesConstant(varName, loc),
							std::move(tmpRead), loc), loc);
					}
					else
					{
						m_ctx.queueStmt(awst::makeAppGlobalPut(
							awst::makeUtf8BytesConstant(varName, loc),
							std::move(cat), loc), loc);
					}

					return awst::makeVoidConstant(loc);
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

		// Chained storage path (`m[k].field.push()`, `arr[i].field.push()`, etc.):
		// unwrap StateGet and emit ArrayExtend/ArrayPop. Only fires when
		// handleStructFieldArrayMethod (simple Identifier case) didn't match.
		auto const* maType = dynamic_cast<ArrayType const*>(
			innerMA->annotation().type);
		if (maType && maType->isDynamicallySized()
			&& !maType->isByteArrayOrString()
			&& (memberName == "push" || memberName == "pop"))
		{
			auto baseAwst = buildExpr(baseExpr);
			baseAwst = awst::makeWritableTarget(baseAwst);

			if (dynamic_cast<awst::BoxValueExpression const*>(baseAwst.get())
				|| dynamic_cast<awst::IndexExpression const*>(baseAwst.get())
				|| dynamic_cast<awst::FieldExpression const*>(baseAwst.get()))
			{
				auto* rawElemType = m_ctx.typeMapper.map(maType->baseType());
				auto* elemType = m_ctx.typeMapper.mapSolTypeToARC4(
					maType->baseType());
				auto* arrWType = baseAwst->wtype
					? baseAwst->wtype : m_ctx.typeMapper.map(maType);

				// Chained mapping-entry push (`m[k].arr.push()`): the lazy per-entry
				// STRUCT box holding the dyn-array field must be materialised (with a
				// valid default struct encoding) before ArrayExtend's box_extract, else
				// "no such box". Same prologue as the m[k].push() branch above.
				if (auto stmt = builder::StorageMapper::makeEnsureRootBoxForWrite(
						m_ctx.typeMapper, baseAwst, /*isResize=*/true, m_loc))
					m_ctx.queuePrePending(std::move(stmt));

				if (memberName == "push" && !m_call.arguments().empty())
				{
					auto val = buildExpr(*m_call.arguments()[0]);
					auto encoded = awst::makeARC4Encode(std::move(val), elemType, m_loc);
					return awst::makeArrayPushOne(
						std::move(baseAwst), std::move(encoded), arrWType, m_loc);
				}
				if (memberName == "push" && m_call.arguments().empty())
				{
					std::shared_ptr<awst::Expression> elem;
					bool fromAssign = static_cast<bool>(m_ctx.pendingArrayPushValue);
					if (fromAssign)
					{
						auto coerced = builder::TypeCoercion::coerceForAssignment(
							std::move(m_ctx.pendingArrayPushValue), rawElemType, m_loc);
						elem = awst::makeARC4Encode(std::move(coerced), elemType, m_loc);
					}
					else
					{
						elem = builder::TypeCoercion::makeDefaultValue(elemType, m_loc);
					}

					auto extend = awst::makeArrayPushOne(baseAwst, std::move(elem), arrWType, m_loc);

					if (fromAssign)
						return extend;

					m_ctx.queuePreStmt(std::move(extend), m_loc);
					return awst::makeVoidConstant(m_loc);
				}
				if (memberName == "pop")
					return awst::makeArrayPopDecode(
						std::move(baseAwst), elemType, rawElemType, m_loc);
			}
		}
	}

	return handleMemoryArray(memberName, baseExpr);
}


} // namespace puyasol::builder::sol_ast
