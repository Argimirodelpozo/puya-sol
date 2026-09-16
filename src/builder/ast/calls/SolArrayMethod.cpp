/// @file SolArrayMethod.cpp
/// array.push(val), array.push(), and array.pop().
/// The solc storage location selects validity; physical carriers select emission.

#include "builder/ast/calls/SolArrayMethod.h"
#include "awst/NameGen.h"
#include "Logger.h"
#include "builder/storage/slot/EvmSlotLowering.h"
#include "builder/eb/ResolvedLValue.h"
#include "builder/solc/SolcFacts.h"
#include "builder/target/EvmLayoutMode.h"
#include "builder/storage/slot/SlotHandleAccess.h"
#include "builder/storage/StorageMapper.h"
#include "builder/types/TypeMapper.h"
#include "builder/types/TypeCoercion.h"
#include "builder/types/ConversionPlan.h"
#include "builder/eb/AssignmentHelper.h"

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
	if (auto const* castCall = SolcFacts::expressionAs<FunctionCall>(&baseExpr))
	{
		if (*castCall->annotation().kind == FunctionCallKind::TypeConversion
			&& castCall->arguments().size() == 1)
		{
			auto const* convArg = &SolcFacts::unparenthesized(*castCall->arguments()[0]);
			if (auto const* convIdent = SolcFacts::expressionAs<Identifier>(convArg))
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

std::shared_ptr<awst::Expression> SolArrayMethod::buildArrayTarget(Expression const& source)
{
	auto operand = m_ctx.lowerOperand([&] {
		auto const* id = SolcFacts::expressionAs<Identifier>(&source);
		auto const* declaration = id ? id->annotation().referencedDeclaration : nullptr;
		auto const* alias = declaration ? m_scope.bindings.storageAliases.find(declaration->id()) : nullptr;
		// Value lowering may substitute a holder placeholder for mapping-containing
		// aliases. Mutation needs their bound lvalue, not that placeholder.
		return ResolvedLValue::freezeTarget(m_ctx,
			awst::makeWritableTarget(alias ? alias->expr : buildExpr(source)), m_loc);
	}, false);
	return m_ctx.emitSequencedOperand(std::move(operand.effects), std::move(operand.value), false, m_loc);
}

std::shared_ptr<awst::Expression> SolArrayMethod::buildPushValue(
	Type const* elementType, awst::WType const* representation)
{
	auto const* native = m_ctx.typeMapper.map(elementType);
	std::shared_ptr<awst::Expression> value;
	if (!m_call.arguments().empty())
	{
		auto const& argument = *m_call.arguments()[0];
		auto operand = m_ctx.lowerOperand([&] {
			auto built = EvmSlotLowering::materializeRefValue(m_ctx, m_scope,
				buildExpr(argument), argument.annotation().type, native, m_loc);
			return ConversionPlan{argument.annotation().type, elementType, native,
				ConversionPlan::Context::Argument}.emit(std::move(built), m_loc, &m_ctx.preEffects());
		}, false);
		value = m_ctx.emitSequencedOperand(
			std::move(operand.effects), std::move(operand.value), true, m_loc);
	}
	else if (m_ctx.hasArrayAssignmentValue())
		value = m_ctx.takeArrayAssignmentValue(); // Converted by the assignment's solc types.
	else
		return TypeCoercion::makeDefaultValue(representation, m_loc);
	return eb::AssignmentHelper::arc4EncodeForType(m_ctx, std::move(value), representation, m_loc);
}

std::shared_ptr<awst::Expression> SolArrayMethod::buildBytesPushPop(
	std::string const& memberName, Expression const& baseExpr, ArrayType const& array)
{
	ResolvedLValue destination(m_ctx, *peelBytesCastBase(baseExpr), m_loc);
	std::shared_ptr<awst::Expression> value;
	if (memberName == "push")
		value = buildPushValue(array.baseType(), m_ctx.typeMapper.map(array.baseType()));
	auto current = m_ctx.emitSequencedOperand({}, destination.read(), true, m_loc);
	if (current->wtype != awst::WType::bytesType())
		current = awst::makeAsBytes(std::move(current), m_loc);
	if (memberName == "push")
		value = awst::makeConcat(current, awst::makeAsBytes(std::move(value), m_loc), m_loc);
	else
	{
		m_ctx.queuePreExpression(awst::makeAssert(awst::makeNumericCompare(
			awst::makeLen(current, m_loc), awst::NumericComparison::Gt, awst::makeZero(m_loc), m_loc),
			m_loc, "pop from empty bytes"), m_loc);
		value = awst::makeExtract3(current, awst::makeZero(m_loc), awst::makeUInt64BinOp(
			awst::makeLen(current, m_loc), awst::UInt64BinaryOperator::Sub, awst::makeOne(m_loc), m_loc), m_loc);
	}
	destination.write(std::move(value));
	return awst::makeVoidConstant(m_loc);
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
	auto rootSlot = m_ctx.emitSequencedOperand({}, base->slot, true, m_loc);
	std::shared_ptr<awst::Expression> value;
	if (memberName == "push" && (!arguments().empty() || m_ctx.hasArrayAssignmentValue()))
		value = buildPushValue(elemType, m_ctx.typeMapper.map(elemType));
	auto len = pin(EvmSlotLowering::readSlotWord(rootSlot, m_loc), "len");
	auto dataBase = EvmSlotLowering::dynDataBase(rootSlot, m_loc);

	if (memberName == "push")
	{
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
	auto zero = TypeCoercion::makeDefaultValue(addr.wtype, m_loc);
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
std::shared_ptr<awst::Expression> SolArrayMethod::emitArrayPushPop(
	std::string const& memberName,
	std::shared_ptr<awst::Expression> baseAwst,
	ArrayType const& solArrType)
{
	auto* arrWType = baseAwst->wtype
		? baseAwst->wtype : m_ctx.typeMapper.map(&solArrType);
	auto const* elemType = awst::arrayElementType(arrWType);
	assert(elemType);
	baseAwst = ResolvedLValue::freezeTarget(m_ctx, std::move(baseAwst), m_loc);
	bool const fromAssign = m_ctx.hasArrayAssignmentValue();
	auto elem = memberName == "push" ? buildPushValue(solArrType.baseType(), elemType) : nullptr;

	if (auto stmt = builder::StorageMapper::makeEnsureRootBoxForWrite(
			m_ctx.typeMapper, baseAwst, /*isResize=*/true, m_loc))
		m_ctx.queuePreEffect(std::move(stmt));

	if (memberName == "pop")
		return awst::makeArrayPop(baseAwst, elemType, m_loc); // Solidity pop has no result.

	if (!m_call.arguments().empty())
		return awst::makeArrayPushOne(baseAwst, std::move(elem), arrWType, m_loc);
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
	return awst::makeIndexExpression(baseAwst,
		m_ctx.emitSequencedOperand({}, std::move(lastIndex), true, m_loc), elemType, m_loc);
}


std::shared_ptr<awst::Expression> SolArrayMethod::toAwst()
{
	auto const& funcExpr = funcExpression();
	auto const* memberAccess = SolcFacts::expressionAs<MemberAccess>(&funcExpr);
	if (!memberAccess)
		return nullptr;

	std::string memberName = memberAccess->memberName();
	auto const& baseExpr = SolcFacts::unparenthesized(memberAccess->expression());
	auto const* array = dynamic_cast<ArrayType const*>(baseExpr.annotation().type);
	assert(array && array->dataStoredIn(DataLocation::Storage));
	if (array->isByteArrayOrString())
		return buildBytesPushPop(memberName, baseExpr, *array);

	// --evm-storage-layout: push/pop on a storage dynamic array (see the
	// slot-mode helpers).
	if (m_ctx.typeMapper.profile().evmStorageLayout && (memberName == "push" || memberName == "pop"))
	{
		auto const* arrT = dynamic_cast<ArrayType const*>(baseExpr.annotation().type);
		if (arrT && arrT->isDynamicallySized()
			&& arrT->dataStoredIn(DataLocation::Storage)
			&& EvmSlotLowering::isStorageStateRef(baseExpr))
		{
			return buildSlotModeArrayPushPop(memberName, baseExpr, arrT);
		}
	}

	if (auto const* ident = SolcFacts::expressionAs<Identifier>(&baseExpr))
	{
		if (auto const* varDecl = dynamic_cast<VariableDeclaration const*>(
				ident->annotation().referencedDeclaration))
		{
			auto const& key = m_scope.bindings.mappingKeyParams.get(varDecl->id());
			if (!key.empty())
				return handleBoxArray(memberName, *varDecl, awst::makeReinterpretCast(
					awst::makeVarExpression(key, awst::WType::bytesType(), m_loc),
					awst::WType::boxKeyType(), m_loc));
			// Generic box-stored dynamic array (non-bytes)
			if (varDecl->isStateVariable()
				&& m_ctx.storageMapper.shouldUseBoxStorage(*varDecl)
				&& dynamic_cast<ArrayType const*>(varDecl->type()))
			{
				return handleBoxArray(memberName, *varDecl);
			}
		}
	}

	return emitArrayPushPop(memberName, buildArrayTarget(baseExpr), *array);
}


} // namespace puyasol::builder::sol_ast
