/// @file SolArrayMethod.cpp
/// array.push(val), array.push(), and array.pop().
/// Box-backed arrays read/write from box storage; memory arrays use AWST nodes directly.

#include "builder/sol-ast/calls/SolArrayMethod.h"
#include "awst/NameGen.h"
#include "Logger.h"
#include "builder/sol-ast/EvmSlotLowering.h"
#include "builder/storage/EvmLayoutMode.h"
#include "builder/storage/SlotHandleAccess.h"
#include "builder/storage/StorageMapper.h"
#include "builder/sol-types/TypeMapper.h"
#include "builder/sol-types/TypeCoercion.h"
#include "builder/sol-types/ConversionPlan.h"
#include "builder/sol-eb/AssignmentHelper.h"

#include <libsolidity/ast/AST.h>

#include <functional>

namespace puyasol::builder::sol_ast
{

using namespace solidity::frontend;

namespace
{
// `bytes(stringStateVar).push(...)/.pop()`: the base AST is
// FunctionCall(TypeConversion,[Identifier]), not a bare Identifier,
// so the state-var paths don't fire. Unwrap to the Identifier.
Expression const* peelBytesCastBase(Expression const& baseExpr)
{
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
	return effectiveBase;
}
} // anonymous namespace

std::shared_ptr<awst::Expression> SolArrayMethod::buildPushValue(
	Type const* elementType, awst::WType const* representation)
{
	auto const* native = m_ctx.typeMapper.map(elementType);
	std::shared_ptr<awst::Expression> value;
	if (!m_call.arguments().empty())
	{
		auto const& argument = *m_call.arguments()[0];
		value = EvmSlotLowering::materializeRefValue(m_ctx, m_scope,
			buildExpr(argument), argument.annotation().type, native, m_loc);
		value = ConversionPlan{argument.annotation().type, elementType, native,
			ConversionPlan::Context::Argument}.emit(std::move(value), m_loc, &m_ctx.preEffects());
	}
	else if (m_ctx.hasArrayAssignmentValue())
		value = m_ctx.takeArrayAssignmentValue(); // Converted by the assignment's solc types.
	else
		return TypeCoercion::makeDefaultValue(representation, m_loc);
	return eb::AssignmentHelper::arc4EncodeForType(m_ctx, std::move(value), representation, m_loc);
}

/// Slot-mode bytes/string push/pop via whole-value read-modify-write: the short↔long form transitions already live in …
std::shared_ptr<awst::Expression> SolArrayMethod::buildSlotModeBytesPushPop(
	std::string const& memberName,
	Expression const& baseExpr,
	ArrayType const* arrT)
{
	// push/pop via whole-value read-modify-write: the short↔long
	// form transitions already live in __evm_bytes_read/write, so
	// appending a byte or shrinking by one needs no new runtime.
	EvmSlotLowering lowB(m_ctx, m_scope, m_loc);
	auto baseB = lowB.resolve(baseExpr);
	if (!baseB)
		return nullptr;
	baseB->slot = awst::makeEvalOnce(baseB->slot, m_loc);
	baseB->solType = arrT;
	auto cur = lowB.readBytesValue(*baseB);
	if (cur && cur->wtype != awst::WType::bytesType())
		cur = awst::makeAsBytes(std::move(cur), m_loc);
	std::string nm = "__evm_bp_" + std::to_string(
		awst::NameGen::next("SolArrayMethod.bytesPP"));
	m_ctx.queuePreEffect(awst::makeAssignmentStatement(
		awst::makeVarExpression(nm, awst::WType::bytesType(), m_loc),
		std::move(cur), m_loc));
	auto curVar = [&]() {
		return awst::makeVarExpression(
			nm, awst::WType::bytesType(), m_loc);
	};
	std::vector<std::shared_ptr<awst::Statement>> writesB;
	if (memberName == "push")
	{
		std::shared_ptr<awst::Expression> b;
		if (!m_call.arguments().empty())
		{
			b = buildExpr(*m_call.arguments()[0]);
			if (!b)
				return nullptr;
			// bytes1 value → its single content byte. A uint64
			// (integer literal / conversion) has no direct bytes
			// cast — itob and take the LOW byte instead.
			if (b->wtype == awst::WType::uint64Type())
				b = awst::makeExtract(
					awst::makeItob(std::move(b), m_loc), 7, 1, m_loc);
			else
			{
				if (b->wtype != awst::WType::bytesType())
					b = awst::makeAsBytes(std::move(b), m_loc);
				b = awst::makeExtract(std::move(b), 0, 1, m_loc);
			}
		}
		else
			b = awst::makeBytesConstant({0}, m_loc);
		lowB.writeBytesValue(*baseB,
			awst::makeConcat(curVar(), std::move(b), m_loc),
			writesB);
	}
	else
	{
		auto lenE = awst::makeLen(curVar(), m_loc);
		auto nonEmptyB = awst::makeNumericCompare(
			awst::makeLen(curVar(), m_loc),
			awst::NumericComparison::Gt,
			awst::makeIntegerConstant(uint64_t{0}, m_loc), m_loc);
		m_ctx.queuePreEffect(awst::makeExpressionStatement(
			awst::makeAssert(std::move(nonEmptyB), m_loc,
				"pop from empty bytes"), m_loc));
		auto newLen = awst::makeUInt64BinOp(
			awst::makeLen(curVar(), m_loc),
			awst::UInt64BinaryOperator::Sub,
			awst::makeIntegerConstant(uint64_t{1}, m_loc), m_loc);
		(void)lenE;
		lowB.writeBytesValue(*baseB,
			awst::makeExtract3(curVar(),
				awst::makeIntegerConstant(uint64_t{0}, m_loc),
				std::move(newLen), m_loc),
			writesB);
	}
	for (auto& stB: writesB)
		m_ctx.queuePreEffect(std::move(stB));
	return awst::makeZero(m_loc, awst::WType::biguintType());
}

/// Slot-mode dynamic-array push/pop: length-word RMW at the root slot + element write at keccak256(slot32)+addressing.
std::shared_ptr<awst::Expression> SolArrayMethod::buildSlotModeArrayPushPop(
	std::string const& memberName,
	Expression const& baseExpr,
	ArrayType const* arrT)
{
	auto const* elemType = arrT->baseType();
	bool mappingElem =
		dynamic_cast<solidity::frontend::MappingType const*>(elemType)
			!= nullptr;
	EvmSlotLowering low(m_ctx, m_scope, m_loc);
	auto base = low.resolve(baseExpr);
	if (!base)
		return nullptr;
	// Root slot and length feed several statements — pin to temps.
	auto pin = [&](std::shared_ptr<awst::Expression> e, char const* tag) {
		if (dynamic_cast<awst::VarExpression const*>(e.get())
			|| dynamic_cast<awst::IntegerConstant const*>(e.get()))
			return e;
		std::string nm = std::string("__evm_") + tag + "_"
			+ std::to_string(awst::NameGen::next("SolArrayMethod.evmPin"));
		auto const* wt = e->wtype;   // read BEFORE the move (arg eval order)
		m_ctx.queuePreEffect(awst::makeAssignmentStatement(
			awst::makeVarExpression(nm, wt, m_loc), std::move(e), m_loc));
		return std::shared_ptr<awst::Expression>(
			awst::makeVarExpression(nm, wt, m_loc));
	};
	auto rootSlot = pin(base->slot, "arr");
	auto len = pin(EvmSlotLowering::readSlotWord(rootSlot, m_loc), "len");
	auto dataBase = EvmSlotLowering::dynDataBase(rootSlot, m_loc);

	if (memberName == "push")
	{
		std::shared_ptr<awst::Expression> value;
		if (!m_call.arguments().empty() || m_ctx.hasArrayAssignmentValue())
			value = buildPushValue(elemType, m_ctx.typeMapper.map(elemType));
		if (mappingElem)
		{
			// push() on a mapping element: nothing to write — its
			// content is addressed by keccak paths, exactly as EVM
			// leaves it.
			auto newLenM = awst::makeBigUIntBinOp(len,
				awst::BigUIntBinaryOperator::Add,
				awst::makeIntegerConstant("1", m_loc,
					awst::WType::biguintType()), m_loc);
			m_ctx.queuePreEffect(builder::SlotHandleAccess::writeSlot(
				rootSlot, std::move(newLenM), m_loc));
			return awst::makeZero(m_loc, awst::WType::biguintType());
		}
		auto addr = low.elemAddr(dataBase, len, elemType);
		// The declared element type owns the dispatch. Scalars, structs,
		// bytes and arbitrarily nested arrays all enter the same recursive
		// writer used by ordinary assignment; push must not maintain a
		// second immediate-shape ladder.
		if (value)
		{
			std::vector<std::shared_ptr<awst::Statement>> writes;
			if (!low.writeAny(addr, elemType, std::move(value), writes))
				return nullptr;
			for (auto& st: writes)
				m_ctx.queuePreEffect(std::move(st));
		}
		auto newLen = awst::makeBigUIntBinOp(len,
			awst::BigUIntBinaryOperator::Add,
			awst::makeIntegerConstant("1", m_loc, awst::WType::biguintType()),
			m_loc);
		m_ctx.queuePreEffect(builder::SlotHandleAccess::writeSlot(
			rootSlot, std::move(newLen), m_loc));
		if (m_call.arguments().empty())
			return addr.slot;
		return awst::makeZero(m_loc, awst::WType::biguintType());
	}

	// pop
	auto nonEmpty = awst::makeNumericCompare(len,
		awst::NumericComparison::Gt,
		awst::makeIntegerConstant("0", m_loc, awst::WType::biguintType()),
		m_loc);
	m_ctx.queuePreEffect(awst::makeExpressionStatement(
		awst::makeAssert(std::move(nonEmpty), m_loc, "pop from empty array"),
		m_loc));
	auto lastIdx = pin(awst::makeBigUIntBinOp(len,
		awst::BigUIntBinaryOperator::Sub,
		awst::makeIntegerConstant("1", m_loc, awst::WType::biguintType()),
		m_loc), "last");
	if (mappingElem)
	{
		// pop: mapping content becomes unreachable, which is what EVM
		// does too (it cannot clear a mapping element either)
		m_ctx.queuePreEffect(builder::SlotHandleAccess::writeSlot(
			rootSlot, lastIdx, m_loc));
		return awst::makeZero(m_loc, awst::WType::biguintType());
	}
	auto addr = low.elemAddr(dataBase, lastIdx, elemType);
	if (!elemType->isValueType())
	{
		addr.solType = elemType;
		addr.wtype = m_ctx.typeMapper.map(elemType);
		std::vector<std::shared_ptr<awst::Statement>> writesA;
		if (!low.clearAggregate(addr, elemType, writesA))
			return nullptr;
		for (auto& stA: writesA)
			m_ctx.queuePreEffect(std::move(stA));
		m_ctx.queuePreEffect(builder::SlotHandleAccess::writeSlot(
			rootSlot, lastIdx, m_loc));
		return awst::makeZero(m_loc, awst::WType::biguintType());
	}
	std::shared_ptr<awst::Expression> zero;
	if (addr.wtype == awst::WType::accountType())
		zero = awst::makeAddressConstant(
			"AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAY5HFKQ", m_loc);
	else if (addr.wtype == awst::WType::biguintType())
		zero = awst::makeZero(m_loc, awst::WType::biguintType());
	else if (auto const* bw = dynamic_cast<awst::BytesWType const*>(addr.wtype);
		bw && bw->length().has_value())
		zero = awst::makeBytesConstant(
			std::vector<uint8_t>(static_cast<size_t>(*bw->length()), 0), m_loc,
			awst::BytesEncoding::Base16, addr.wtype);
	else if (addr.wtype == awst::WType::boolType())
		zero = awst::makeBoolConstant(false, m_loc, awst::WType::boolType());
	else
		zero = awst::makeZero(m_loc);
	std::vector<std::shared_ptr<awst::Statement>> writes;
	low.writeValue(addr, std::move(zero), writes);
	for (auto& st: writes)
		m_ctx.queuePreEffect(std::move(st));
	m_ctx.queuePreEffect(builder::SlotHandleAccess::writeSlot(
		rootSlot, lastIdx, m_loc));
	return awst::makeZero(m_loc, awst::WType::biguintType());
}

/// Shared ARC4 dynamic-array push/pop for a writable storage base
/// (BoxValueExpression / IndexExpression / FieldExpression): the boxed,
/// storage-pointer-alias and chained-field paths used to carry three copies
/// that disagreed (only the boxed one returned the new element reference, so
/// `alias.push().f = v` and `m[k].arr.push().f = v` were compile errors; the
/// alias copy hand-rolled the root-box check). One emitter: ensure the root
/// box before any resize (an empty header, so a pop on it panics like
/// Solidity's pop on an empty array), and a no-argument push returns the
/// reference to the new element.
std::shared_ptr<awst::Expression> SolArrayMethod::emitArc4PushPop(
	std::string const& memberName,
	std::shared_ptr<awst::Expression> baseAwst,
	ArrayType const& solArrType)
{
	auto* rawElemType = m_ctx.typeMapper.map(solArrType.baseType());
	auto* arrWType = baseAwst->wtype
		? baseAwst->wtype : m_ctx.typeMapper.map(&solArrType);
	auto const* elemType = static_cast<awst::ARC4DynamicArray const*>(arrWType)->elementType();

	if (auto stmt = builder::StorageMapper::makeEnsureRootBoxForWrite(
			m_ctx.typeMapper, baseAwst, /*isResize=*/true, m_loc))
		m_ctx.queuePreEffect(std::move(stmt));

	if (memberName == "pop")
		return awst::makeArrayPopDecode(baseAwst, elemType, rawElemType, m_loc);

	if (!m_call.arguments().empty())
		return awst::makeArrayPushOne(baseAwst,
			buildPushValue(solArrType.baseType(), elemType), arrWType, m_loc);
	bool const fromAssign = m_ctx.hasArrayAssignmentValue();
	auto elem = buildPushValue(solArrType.baseType(), elemType);
	auto extend = awst::makeArrayPushOne(baseAwst, std::move(elem), arrWType, m_loc);
	if (fromAssign)
		return extend;

	// The extend runs before the enclosing statement; `arr.push().field = v`
	// then addresses ArrayLength-1.
	m_ctx.queuePreExpression(std::move(extend), m_loc);
	auto lastIndex = awst::makeUInt64BinOp(
		awst::makeArrayLength(baseAwst, awst::WType::uint64Type(), m_loc),
		awst::UInt64BinaryOperator::Sub,
		awst::makeIntegerConstant("1", m_loc),
		m_loc);
	return awst::makeIndexExpression(baseAwst, std::move(lastIndex), elemType, m_loc);
}

/// `m[k].push()/.pop()`: IndexAccess base lowers to BoxValueExpression (wrapped in StateGet when read).
std::shared_ptr<awst::Expression> SolArrayMethod::tryBoxedElementPushPop(
	std::string const& memberName,
	Expression const& baseExpr)
{
	auto const* innerIA = dynamic_cast<IndexAccess const*>(&baseExpr);
	if (!innerIA)
		return nullptr;
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
			return emitArc4PushPop(memberName, std::move(baseAwst), *innerArrType);
	}
	return nullptr;
}

/// Storage-pointer alias / mapping-key-param arrays: push/pop through the aliased BOX (runtime key).
std::shared_ptr<awst::Expression> SolArrayMethod::tryStoragePointerPushPop(
	std::string const& memberName,
	Expression const& baseExpr)
{
	auto const* ident = dynamic_cast<Identifier const*>(&baseExpr);
	if (!ident)
		return nullptr;
	if (auto const* decl = dynamic_cast<VariableDeclaration const*>(
			ident->annotation().referencedDeclaration))
	{
		if (auto const* array = dynamic_cast<ArrayType const*>(decl->type());
			array && array->isDynamicallySized()
			&& !array->isByteArrayOrString()
			&& (memberName == "push" || memberName == "pop"))
		{
			auto const& keyParam = m_scope.bindings.mappingKeyParams.get(decl->id());
			if (!keyParam.empty())
				return handleBoxArray(
					memberName, baseExpr, *decl,
					awst::makeReinterpretCast(
						awst::makeVarExpression(
							keyParam, awst::WType::bytesType(), m_loc),
						awst::WType::boxKeyType(), m_loc));
		}
		if (!decl->isStateVariable())
		{
			auto const* alias = m_scope.bindings.storageAliases.find(decl->id());
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
							m_ctx.queuePostEffect(awst::makeAssignmentStatement(tmpTarget,
								awst::makeConcat(std::move(readVal), std::move(pushVal), loc),
								loc));
						}
						else
						{
							auto newLen = awst::makeUInt64BinOp(
								awst::makeLen(readVal, loc),
								awst::UInt64BinaryOperator::Sub, awst::makeOne(loc), loc);
							m_ctx.queuePostEffect(awst::makeAssignmentStatement(tmpTarget,
								awst::makeExtract3(readVal, awst::makeZero(loc),
									std::move(newLen), loc),
								loc));
						}
						m_ctx.queuePostExpression(awst::makeBoxDel(bv->key, loc), loc);
						m_ctx.queuePostExpression(awst::makeBoxPut(bv->key,
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
						return emitArc4PushPop(memberName, std::move(aliasExpr), *solArrType);
				}
			}
		}
	}
	return nullptr;
}

/// bytes/string STATE VAR push/pop: concat-based push / read+substring+write pop (box_del+box_put for exact-size box rewrite).
std::shared_ptr<awst::Expression> SolArrayMethod::tryStateBytesPushPop(
	std::string const& memberName,
	solidity::frontend::VariableDeclaration const& _varDecl)
{
	auto const* varDecl = &_varDecl;
	// bytes/string state variable: pop = read + substring + write
	if (varDecl->isStateVariable()
		&& varDecl->type()->category() == Type::Category::Array)
	{
		auto const* arrType2 = dynamic_cast<ArrayType const*>(varDecl->type());
		if (arrType2 && arrType2->isByteArrayOrString() && memberName == "pop")
		{
			auto binding = m_ctx.storageMapper.physicalBindingFor(*varDecl);
			std::string varName = binding.key;
			auto loc = m_loc;
			auto kind = binding.kind;

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
				m_ctx.queuePostEffect(awst::makeAssignmentStatement(tmpTarget, std::move(extract), loc));

				m_ctx.queuePostExpression(awst::makeBoxDel(awst::makeUtf8BytesConstant(varName, loc), loc), loc);

				auto tmpRead = awst::makeVarExpression(tmpName, awst::WType::bytesType(), loc);
				m_ctx.queuePostExpression(awst::makeBoxPut(
					awst::makeUtf8BytesConstant(varName, loc),
					std::move(tmpRead), loc), loc);
			}
			else
			{
				m_ctx.queuePostExpression(awst::makeAppGlobalPut(
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
			auto binding = m_ctx.storageMapper.physicalBindingFor(*varDecl);
			std::string varName = binding.key;
			auto loc = m_loc;
			auto kind = binding.kind;

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
				m_ctx.queuePostEffect(awst::makeAssignmentStatement(tmpTarget, std::move(cat), loc));

				m_ctx.queuePostExpression(awst::makeBoxDel(awst::makeUtf8BytesConstant(varName, loc), loc), loc);

				auto tmpRead = awst::makeVarExpression(tmpName, awst::WType::bytesType(), loc);
				m_ctx.queuePostExpression(awst::makeBoxPut(
					awst::makeUtf8BytesConstant(varName, loc),
					std::move(tmpRead), loc), loc);
			}
			else
			{
				m_ctx.queuePostExpression(awst::makeAppGlobalPut(
					awst::makeUtf8BytesConstant(varName, loc),
					std::move(cat), loc), loc);
			}

			return awst::makeVoidConstant(loc);
		}
	}

	return nullptr;
}

/// Chained storage path (`m[k].field.push()`, `arr[i].field.push()`, etc.): unwrap StateGet and emit ArrayExtend/ArrayPop.
std::shared_ptr<awst::Expression> SolArrayMethod::tryChainedFieldPushPop(
	std::string const& memberName,
	Expression const& baseExpr,
	MemberAccess const& innerMA)
{
	// Chained storage path (`m[k].field.push()`, `arr[i].field.push()`, etc.):
	// unwrap StateGet and emit ArrayExtend/ArrayPop. Only fires when
	// handleStructFieldArrayMethod (simple Identifier case) didn't match.
	auto const* maType = dynamic_cast<ArrayType const*>(
		innerMA.annotation().type);
	if (maType && maType->isDynamicallySized()
		&& !maType->isByteArrayOrString()
		&& (memberName == "push" || memberName == "pop"))
	{
		auto baseAwst = buildExpr(baseExpr);
		baseAwst = awst::makeWritableTarget(baseAwst);
		if (dynamic_cast<awst::BoxValueExpression const*>(baseAwst.get())
			|| dynamic_cast<awst::IndexExpression const*>(baseAwst.get())
			|| dynamic_cast<awst::FieldExpression const*>(baseAwst.get()))
			return emitArc4PushPop(memberName, std::move(baseAwst), *maType);
	}
	return nullptr;
}

std::shared_ptr<awst::Expression> SolArrayMethod::toAwst()
{
	auto const& funcExpr = funcExpression();
	auto const* memberAccess = dynamic_cast<MemberAccess const*>(&funcExpr);
	if (!memberAccess)
		return nullptr;

	std::string memberName = memberAccess->memberName();
	auto const& baseExpr = memberAccess->expression();

	// --evm-storage-layout: push/pop on a storage dynamic array (see the
	// slot-mode helpers).
	if (m_ctx.typeMapper.profile().evmStorageLayout && (memberName == "push" || memberName == "pop"))
	{
		auto const* arrT = dynamic_cast<ArrayType const*>(baseExpr.annotation().type);
		if (arrT && arrT->isDynamicallySized()
			&& arrT->dataStoredIn(DataLocation::Storage)
			&& EvmSlotLowering::isStorageStateRef(baseExpr))
		{
			if (arrT->isByteArrayOrString())
				return buildSlotModeBytesPushPop(memberName, baseExpr, arrT);
			return buildSlotModeArrayPushPop(memberName, baseExpr, arrT);
		}
	}

	if (auto result = tryBoxedElementPushPop(memberName, baseExpr))
		return result;

	if (auto result = tryStoragePointerPushPop(memberName, baseExpr))
		return result;

	Expression const* effectiveBase = peelBytesCastBase(baseExpr);

	if (auto const* ident = dynamic_cast<Identifier const*>(effectiveBase))
	{
		if (auto const* varDecl = dynamic_cast<VariableDeclaration const*>(
				ident->annotation().referencedDeclaration))
		{
			if (auto result = tryStateBytesPushPop(memberName, *varDecl))
				return result;

			// Generic box-stored dynamic array (non-bytes)
			if (varDecl->isStateVariable()
				&& m_ctx.storageMapper.shouldUseBoxStorage(*varDecl)
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

		if (auto result = tryChainedFieldPushPop(memberName, baseExpr, *innerMA))
			return result;
	}

	return handleMemoryArray(memberName, baseExpr);
}


} // namespace puyasol::builder::sol_ast
