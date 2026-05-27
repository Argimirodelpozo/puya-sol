/// @file SolArrayMethod.cpp
/// array.push(val), array.push(), and array.pop().
/// Box-backed arrays read/write from box storage; memory arrays use AWST nodes directly.

#include "builder/sol-ast/calls/SolArrayMethod.h"
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
			// Unwrap any StateGet wrapper through the access chain
			// (IndexExpression / FieldExpression of any depth). The
			// rewritten chain bottoms out at the BoxValueExpression so
			// puya's IR accepts it as a writable target.
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

				// Ensure the per-entry box exists with an empty ARC4
				// dynamic-array header (`0x0000`) before ArrayExtend/ArrayPop
				// reads it. Guarded by `box_len.exists` so subsequent pushes
				// (which grew the box past 2 bytes) skip the create.
				auto emitEnsureBox = [&]() {
					// Nested case (IndexExpression base): the outer box
					// already holds the whole multi-dim array; no per-entry
					// box to create.
					// Storage-pointer alias path: the SolIdentifier resolution
					// returns the alias's `expr` which is typically
					// `StateGet(BoxValueExpression(...))`. Unwrap StateGet so
					// the underlying box-creation runs against the aliased
					// slot pointer — without this, `A(m[1])` followed by
					// `m.push()` in A's body silently skips the pre-create and
					// trips the puya ArrayExtend's box-exists assertion.
					auto unwrapped = baseAwst;
					if (auto sg = std::dynamic_pointer_cast<awst::StateGet>(unwrapped))
						unwrapped = sg->field;
					auto const* bv = dynamic_cast<awst::BoxValueExpression const*>(unwrapped.get());
					if (!bv || !bv->key)
						return;
					auto boxKey = bv->key;

					auto* tupleType = m_ctx.typeMapper.createType<awst::WTuple>(
						std::vector<awst::WType const*>{
							awst::WType::uint64Type(), awst::WType::boolType()});
					auto boxLen = awst::makeBoxLen(boxKey, tupleType, m_loc);

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

					m_ctx.queuePrePending(awst::makeIfElse(
						std::move(notExists), std::move(ifBranch), nullptr, m_loc));
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

					// Use queuePreStmt so the extend runs BEFORE the
					// enclosing statement. For `arr.push().field = v` the
					// field write reads ArrayLength - 1 post-extend.
					m_ctx.queuePreStmt(std::move(e), m_loc);

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
					return awst::makeArrayPopDecode(baseAwst, elemType, rawElemType, m_loc);
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
				auto const* alias = m_scope.findStorageAlias(decl->id());
				if (alias
					&& (memberName == "push" || memberName == "pop"))
				{
					auto const* solArrType = dynamic_cast<ArrayType const*>(decl->type());
					if (solArrType && !solArrType->isByteArrayOrString())
					{
						std::shared_ptr<awst::Expression> aliasExpr = alias->expr;
						// Unwrap StateGet (top-level, and any nested in the
						// access chain) to underlying writable target. Same
						// transform used by the `m[k].push()` path above.
						if (auto const* sg = dynamic_cast<awst::StateGet const*>(
								aliasExpr.get()))
							aliasExpr = sg->field;
						aliasExpr = awst::makeWritableTarget(aliasExpr);
						// Underlying targets we can write through:
						//   - BoxValueExpression  (simple `T[] storage p = state;`)
						//   - IndexExpression  (e.g. `T[] storage p = a[i];` — nested
						//     element of a state container, write-back via outer
						//     read-modify-write)
						//   - FieldExpression  (e.g. `T[] storage p = s.field;`)
						if (dynamic_cast<awst::BoxValueExpression const*>(aliasExpr.get())
							|| dynamic_cast<awst::IndexExpression const*>(aliasExpr.get())
							|| dynamic_cast<awst::FieldExpression const*>(aliasExpr.get()))
						{
							auto* rawElemType = m_ctx.typeMapper.map(solArrType->baseType());
							auto* elemType = m_ctx.typeMapper.mapSolTypeToARC4(solArrType->baseType());
							auto* arrWType = aliasExpr->wtype
								? aliasExpr->wtype
								: m_ctx.typeMapper.map(solArrType);

							// Ensure the aliased per-entry box exists with the
							// empty ARC4 dyn-array header (0x0000) before
							// ArrayExtend/ArrayPop reads it. Mirrors the
							// emitEnsureBox helper used in the `m[k].push()`
							// path above — without this, `A(state[k])`
							// followed by `m.push()` in A's body trips puya's
							// box-exists assertion because the aliased box was
							// never created on first access.
							auto emitEnsureAliasBox = [&]() {
								auto const* bv = dynamic_cast<awst::BoxValueExpression const*>(aliasExpr.get());
								if (!bv || !bv->key)
									return;
								auto boxKey = bv->key;
								auto* tupleType = m_ctx.typeMapper.createType<awst::WTuple>(
									std::vector<awst::WType const*>{
										awst::WType::uint64Type(), awst::WType::boolType()});
								auto boxLen = awst::makeBoxLen(boxKey, tupleType, m_loc);
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

					auto one = awst::makeOne(loc);
					auto newLen = awst::makeUInt64BinOp(std::move(lenCall), awst::UInt64BinaryOperator::Sub, std::move(one), loc);

					// extract3(readVal, 0, len-1)
					auto zero = awst::makeZero(loc);

					auto extract = awst::makeExtract3(readVal, std::move(zero), std::move(newLen), loc);
					if (kind == awst::AppStorageKind::Box)
					{
						// Box: store shrunk in temp, box_del, box_put
						static int popTmpCounter = 0;
						std::string tmpName = "__bytes_pop_tmp_" + std::to_string(popTmpCounter++);

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
						// Box: store concat in temp, box_del, box_put(key, temp)
						// box_put requires exact size match, so we delete+recreate
						static int tmpCounter = 0;
						std::string tmpName = "__bytes_push_tmp_" + std::to_string(tmpCounter++);

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

		// Chained storage path with field access: `m[k].field.push()`,
		// `arr[i].field.push()`, `s.inner.field.push()` etc. — the base
		// MemberAccess wraps over IndexAccess / nested MemberAccess.
		// Same approach as the IndexAccess branch above: build the read,
		// unwrap any StateGet inside the chain, hand off to ArrayExtend /
		// ArrayPop on the resulting writable target. handleStructFieldArrayMethod
		// already covers the simple Identifier inner case, so this only
		// fires when that didn't match.
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
