/// @file SolAssignment.cpp
/// Top-level assignment translator (try*/apply* pipeline).
/// Shape-specific handlers live in sibling SolAssignment*.cpp.

#include <algorithm>
#include "builder/ast/exprs/SolAssignment.h"
#include "builder/ast/exprs/SolTupleExpression.h"
#include "builder/eb/ResolvedLValue.h"
#include "builder/solc/SolcFacts.h"
#include "awst/NameGen.h"
#include "builder/eb/AssignmentHelper.h"
#include "builder/storage/StorageMapper.h"
#include "builder/storage/TransientStorage.h"
#include "builder/types/TypeMapper.h"
#include "builder/types/SolIntType.h"
#include "builder/codec/Arc4Defaults.h"
#include "builder/types/TypeCoercion.h"
#include "builder/types/ConversionPlan.h"
#include "builder/yul/AssemblyBuilder.h"
#include "builder/eb/EffectScan.h"
#include "builder/storage/slot/EvmSlotLowering.h"
#include "builder/contract/ContractBuilder.h"
#include "builder/codec/EvmMemoryCodec.h"
#include "builder/codec/EvmValueCodec.h"
#include "builder/ast/exprs/SolIndexAccess.h"
#include "builder/target/EvmLayoutMode.h"
#include "builder/storage/slot/SlotHandleAccess.h"
#include "builder/codec/SlotWordCodec.h"
#include "Logger.h"

#include <libsolidity/ast/AST.h>

namespace puyasol::builder::sol_ast
{

using namespace solidity::frontend;
using Token = solidity::frontend::Token;

SolAssignment::SolAssignment(eb::ContractContext& _ctx, Assignment const& _node)
	: SolExpression(_ctx, _node), m_assignment(_node)
{
}

// toAwst pipeline:
//   (1) Pre-buildExpr early-outs (transient, storage-ptr, multi-box, push-assign)
//   (2) Build target + value
//   (3) Build an LValuePlan and dispatch the single applicable write strategy.
std::shared_ptr<awst::Expression> SolAssignment::toAwst()
{
	Token op = m_assignment.assignmentOperator();

	// (1) Pre-buildExpr early-outs.
	// A reference-typed leaf of a scratch-model aggregate is a pointer slot;
	// it must not reach the addressed in-place writer below.
	if (auto r = tryHandleScratchReferenceSlotWrite()) return std::move(*r);
	if (auto r = tryHandleAddressedWrite())         return std::move(*r);
	if (auto r = tryHandleEvmStorageWrite())         return std::move(*r);
	if (auto r = tryHandleBlobRespill())             return std::move(*r);
	if (auto r = tryHandleStoragePointerReassign())  return std::move(*r);
	if (auto r = tryHandlePushAssignRewrite(op))     return std::move(*r);

	// (2) Build target + value (if tryHandlePushAssignRewrite claimed, it already returned).
	// Solc's ExpressionCompiler and IRGeneratorForStatements both evaluate the
	// RHS fully FIRST, for plain and compound assignments (`arr[j++] = j`
	// stores the pre-increment j). Capture both sides' queued effects
	// and re-emit RHS-first: RHS pre, a pin of the RHS value, RHS write-backs
	// (hoisted before the store), then the LHS effects. Effect-free sides
	// re-emit byte-identically with no pin. Tuple assignments keep the plain
	// build (their element-wise handler owns sequencing).
	std::shared_ptr<awst::Expression> target, value;
	auto const* sourceLhs = dynamic_cast<TupleExpression const*>(&m_assignment.leftHandSide());
	while (sourceLhs && sourceLhs->components().size() == 1 && sourceLhs->components()[0])
		sourceLhs = dynamic_cast<TupleExpression const*>(sourceLhs->components()[0].get());
	if (sourceLhs && dynamic_cast<TupleType const*>(sourceLhs->annotation().type))
	{
		std::vector<VariableDeclaration const*> bindings;
		for (auto const& component: sourceLhs->components())
		{
			auto const* id = component ? dynamic_cast<Identifier const*>(&SolcFacts::functionExpression(*component)) : nullptr;
			bindings.push_back(id ? dynamic_cast<VariableDeclaration const*>(id->annotation().referencedDeclaration) : nullptr);
		}
		// Solc snapshots RHS values, evaluates LHS addresses left-to-right, then
		// writes components right-to-left. Keep each resolved address for its store.
		auto rhs = m_ctx.lowerOperand([&] {
			return pinLiteralTupleRhs(snapshotTupleCallRhs(
				SolTupleExpression::buildBindingRhs(m_ctx, m_assignment.rightHandSide(), bindings)), sourceLhs);
		}, false);
		value = m_ctx.emitSequencedOperand(std::move(rhs.effects), std::move(rhs.value), false, m_loc);
		std::function<std::shared_ptr<awst::Expression>(Expression const&)> resolve;
		resolve = [&](Expression const& component) -> std::shared_ptr<awst::Expression> {
			if (auto const* nested = dynamic_cast<TupleExpression const*>(&component))
			{
				if (nested->components().size() == 1 && nested->components()[0])
					return resolve(*nested->components()[0]);
				auto tuple = awst::makeTupleExpression(nullptr, m_loc);
				std::vector<awst::WType const*> types;
				for (auto const& leaf: nested->components())
				{
					auto item = leaf ? resolve(*leaf) : awst::makeVarExpression("", awst::WType::uint64Type(), m_loc);
					types.push_back(item->wtype);
					tuple->items.push_back(std::move(item));
				}
				tuple->wtype = m_ctx.typeMapper.createType<awst::WTuple>(std::move(types), std::nullopt);
				return tuple;
			}
			auto const* identifier = dynamic_cast<Identifier const*>(&component);
			auto const* declaration = identifier
				? dynamic_cast<VariableDeclaration const*>(identifier->annotation().referencedDeclaration) : nullptr;
			auto resolution = ResolvedLValue::classify(m_ctx, component);
			if (component.annotation().type->isValueType()
				|| ((!declaration || declaration->isStateVariable()) && resolution.isAddressed()))
			{
				m_tupleTargets.emplace(component.id(), std::make_shared<ResolvedLValue>(m_ctx, component, m_loc, std::move(resolution)));
				return awst::makeVarExpression("__tuple_destination",
					m_ctx.typeMapper.map(component.annotation().type), m_loc);
			}
			auto lhs = m_ctx.lower(component, false);
			return m_ctx.emitSequencedOperand(std::move(lhs.effects), std::move(lhs.value), false, m_loc);
		};
		return handleTupleAssignment(resolve(*sourceLhs), std::move(value), sourceLhs);
	}
	else
	{
		eb::ContractContext::OperandDeltas lhsD, rhsD;
		auto rhs = m_ctx.lowerOperand([&] {
			auto value = buildExpr(m_assignment.rightHandSide());
			if (m_assignment.leftHandSide().annotation().type->dataStoredIn(DataLocation::Memory))
				value = StorageMapper::makePartialBoxReadWithDefault(
					m_ctx.typeMapper, std::move(value), m_ctx.preEffects(), m_loc);
			return value;
		}, false);
		auto lhs = m_ctx.lower(m_assignment.leftHandSide(), false);
		target = std::move(lhs.value);
		value = std::move(rhs.value);
		lhsD = std::move(lhs.effects);
		rhsD = std::move(rhs.effects);
		// A writing RHS must finish before the target's key/index reads;
		// a writing LHS index needs the RHS value frozen first.
		bool lhsPlainLocal = false;
		if (auto const* lid = dynamic_cast<Identifier const*>(&m_assignment.leftHandSide()))
			if (auto const* lvd = dynamic_cast<VariableDeclaration const*>(
					lid->annotation().referencedDeclaration))
				lhsPlainLocal = lvd->isLocalVariable()
					&& !lvd->type()->dataStoredIn(DataLocation::Storage);
		bool staticNeed =
			(builder::EffectScan::requiresSequencing(m_assignment.rightHandSide(), m_ctx) && !lhsPlainLocal)
			|| builder::EffectScan::requiresSequencing(m_assignment.leftHandSide(), m_ctx);
		bool reorder = !lhsD.empty() || !rhsD.post.empty() || staticNeed;
		value = m_ctx.emitSequencedOperand(std::move(rhsD), std::move(value), reorder, m_loc);
		// Index lowering already materializes side-effecting indexes. Finish
		// their write-backs before reading/storing the target, never after the
		// assignment (which would overwrite it). Keep the lvalue itself unpinned.
		target = m_ctx.emitSequencedOperand(std::move(lhsD), std::move(target), false, m_loc);
	}

	if (!target || !value) return nullptr;

	// Solc-convertibility tripwire: a plain `=` must be a solc-legal
	// implicit conversion; a trip = wrong src/target annotation plumbing.
	// Compound ops follow binaryOperatorResult rules instead — skip; tuples
	// compare element-wise — skip.
	if (op == Token::Assign
		&& m_assignment.rightHandSide().annotation().type
		&& !dynamic_cast<TupleType const*>(m_assignment.rightHandSide().annotation().type)
		&& !dynamic_cast<TupleType const*>(m_assignment.leftHandSide().annotation().type))
		builder::TypeCoercion::assertImplicitlyConvertible(
			m_assignment.rightHandSide().annotation().type,
			m_assignment.leftHandSide().annotation().type, m_loc, "assignment");

	// (3) Classify once, then execute the selected write strategy.
	value = applyEnumRangeCheck(std::move(value), op);
	auto lvaluePlan = planLValue(target);
	return emitLValuePlan(
		lvaluePlan, op, std::move(target), std::move(value));
}

SolAssignment::LValuePlan SolAssignment::planLValue(
	std::shared_ptr<awst::Expression> const& _target) const
{
	if (dynamic_cast<awst::TupleExpression const*>(_target.get()))
		return {LValueKind::Tuple};

	if (_target->wtype == awst::WType::biguintType())
	{
		auto const* lhsType = m_assignment.leftHandSide().annotation().type;
		auto const* rhsType = m_assignment.rightHandSide().annotation().type;
		auto const* lhsArray = lhsType ? dynamic_cast<ArrayType const*>(lhsType) : nullptr;
		auto const* rhsArray = rhsType ? dynamic_cast<ArrayType const*>(rhsType) : nullptr;
		// Keep this selection identical to trySlotBasedArrayWrite: an array-typed
		// LHS owns the strategy decision; only a non-array LHS may borrow the RHS
		// array type.  In particular, dynamic-LHS/fixed-RHS must continue to the
		// scalar-slot strategy exactly as the former specialist chain did.
		auto const* arrayType = lhsArray ? lhsArray : rhsArray;
		if (arrayType && !arrayType->isDynamicallySized())
			return {LValueKind::SlotArray};
		if (dynamic_cast<awst::BigUIntBinaryOperation const*>(_target.get()))
			return {LValueKind::SlotScalar};
	}

	return {LValueKind::Generic};
}

std::shared_ptr<awst::Expression> SolAssignment::emitLValuePlan(
	LValuePlan _plan,
	Token _op,
	std::shared_ptr<awst::Expression> _target,
	std::shared_ptr<awst::Expression> _value)
{
	std::optional<std::shared_ptr<awst::Expression>> result;
	switch (_plan.kind)
	{
	case LValueKind::SlotArray:
		result = trySlotBasedArrayWrite(_op, _target, _value);
		break;
	case LValueKind::SlotScalar:
		result = trySlotBasedScalarWrite(_op, _target, _value);
		break;
	case LValueKind::Tuple:
		result = tryTupleAssignment(_target, _value);
		break;
	case LValueKind::Generic:
		break;
	}


	if (result)
		return std::move(*result);
	return emitGenericAssignment(_op, std::move(_target), std::move(_value));
}

std::shared_ptr<awst::Expression> SolAssignment::emitGenericAssignment(
	Token _op,
	std::shared_ptr<awst::Expression> _target,
	std::shared_ptr<awst::Expression> _value)
{
	ResolvedLValue target(m_ctx, m_assignment.leftHandSide(), m_loc, std::move(_target));
	_value = computeAggregateStoreValue(_op, _op == Token::Assign ? nullptr : target.read(),
		std::move(_value), m_ctx.typeMapper.map(m_assignment.leftHandSide().annotation().type));
	return target.write(std::move(_value));
}

std::optional<std::shared_ptr<awst::Expression>>
SolAssignment::tryHandlePushAssignRewrite(Token _op)
{
	// `arr.push() = value`: scope the RHS while lowering the LHS call so
	// SolArrayMethod::push folds it into ArrayExtend (we don't model Solidity refs).
	if (_op != Token::Assign) return std::nullopt;
	auto const* lhsCall = dynamic_cast<FunctionCall const*>(&m_assignment.leftHandSide());
	if (!lhsCall || !lhsCall->arguments().empty()) return std::nullopt;
	auto const* member = dynamic_cast<MemberAccess const*>(&lhsCall->expression());
	if (!member || member->memberName() != "push") return std::nullopt;

	auto rhs = m_ctx.lowerOperand([&] {
		auto const& source = m_assignment.rightHandSide();
		auto const* native = m_ctx.typeMapper.map(lhsCall->annotation().type);
		auto value = EvmSlotLowering::materializeRefValue(
			m_ctx, m_scope, buildExpr(source), source.annotation().type, native, m_loc);
		return computeAggregateStoreValue(Token::Assign, nullptr, std::move(value), native);
	}, false);
	auto pushValue = m_ctx.emitSequencedOperand(
		std::move(rhs.effects), std::move(rhs.value), true, m_loc);
	auto pushScope = m_ctx.pushArrayAssignmentValue(std::move(pushValue));
	auto target = buildExpr(m_assignment.leftHandSide());
	return target; // ArrayExtend emitted by SolArrayMethod
}

std::optional<std::shared_ptr<awst::Expression>>
SolAssignment::tryHandleAddressedWrite()
{
	auto const& lhs = m_assignment.leftHandSide();
	auto const* type = lhs.annotation().type;
	if (!type) return std::nullopt;
	auto resolution = ResolvedLValue::classify(m_ctx, lhs);
	// Aggregate storage copies and memory-local rebinds retain their dedicated
	// copy/reference policies. Leaves all share one address/read/write path.
	bool const memoryLeaf = type && type->dataStoredIn(DataLocation::Memory)
		&& !dynamic_cast<Identifier const*>(&SolcFacts::functionExpression(lhs));
	if ((!type->isValueType() && !memoryLeaf && !resolution.isBoxedAggregate())
		|| !resolution.isAddressed()) return std::nullopt;
	auto rhs = m_ctx.lower(m_assignment.rightHandSide(), false);
	auto value = m_ctx.emitSequencedOperand(std::move(rhs.effects), std::move(rhs.value), true, m_loc);
	ResolvedLValue target(m_ctx, lhs, m_loc, std::move(resolution));
	auto op = m_assignment.assignmentOperator();
	value = computeAggregateStoreValue(op, op == Token::Assign ? nullptr : target.read(),
		std::move(value), m_ctx.typeMapper.map(type));
	return target.write(std::move(value));
}

std::shared_ptr<awst::Expression>
SolAssignment::applyEnumRangeCheck(std::shared_ptr<awst::Expression> _value, Token _op)
{
	// EVM panic 0x21 on out-of-range enum assign; pre-emit assert.
	if (_op != Token::Assign) return _value;
	auto const* lhsType = m_assignment.leftHandSide().annotation().type;
	auto const* enumType = dynamic_cast<EnumType const*>(lhsType);
	if (!enumType) return _value;

	unsigned numMembers = enumType->numberOfMembers();
	// EvalOnce: the value is referenced by the queued assert AND the returned
	// assignment value — a call-valued RHS ran twice (its twins in
	// SolExpressionStatement/SolEmitStatement already carry this fix).
	_value = awst::makeEvalOnce(std::move(_value), m_loc);
	auto val = builder::TypeCoercion::implicitNumericCast(_value, awst::WType::uint64Type(), m_loc);
	m_ctx.queuePreExpression(awst::makeEnumRangeAssert(val, numMembers, m_loc), m_loc);
	return val;
}

std::optional<std::shared_ptr<awst::Expression>>
SolAssignment::tryHandleBlobRespill()
{
	if (m_assignment.assignmentOperator() != Token::Assign)
		return std::nullopt;
	auto const* lid = dynamic_cast<Identifier const*>(&m_assignment.leftHandSide());
	if (!lid)
		return std::nullopt;
	auto const* lvd = dynamic_cast<VariableDeclaration const*>(
		lid->annotation().referencedDeclaration);
	if (!lvd
		|| lvd->referenceLocation() != VariableDeclaration::Location::Memory
		|| m_scope.bindings.blobAggregates.get(lvd->id()).empty())
		return std::nullopt;
	if (auto reference = SolIndexAccess::resolveBlobReference(
		m_ctx, m_scope, m_assignment.rightHandSide(), m_loc))
	{
		auto offset = m_ctx.emitSequencedOperand(std::move(reference->effects),
			std::move(reference->value), true, m_loc);
		m_ctx.preEffects().push_back(awst::makeAssignmentStatement(awst::makeVarExpression(
			m_scope.bindings.blobAggregates.get(lvd->id()), awst::WType::uint64Type(), m_loc), offset, m_loc));
		return SolIndexAccess::readBlobValue(m_ctx, std::move(offset), lvd->type(), m_loc);
	}
	// Blob-backing is selected per declaration whenever Yul observes an EVM
	// pointer, not only by a universal memory profile. Therefore
	// every such high-level re-assignment must re-spill/repoint the backing
	// offset; falling through would attempt to assign to a materialized value
	// expression (and is not a valid lvalue).
	auto lowered = m_ctx.lower(m_assignment.rightHandSide(), false);
	auto value = m_ctx.emitSequencedOperand(std::move(lowered.effects), std::move(lowered.value), true, m_loc);
	value = ConversionPlan{m_assignment.rightHandSide().annotation().type, lvd->type(),
		m_ctx.typeMapper.map(lvd->type()), ConversionPlan::Context::Assignment}.emit(
			std::move(value), m_loc, &m_ctx.preEffects());
	value = m_ctx.emitSequencedOperand({}, std::move(value), true, m_loc);
	if (!builder::emitBlobBackValue(m_ctx.typeMapper, lvd->type(),
			m_ctx.typeMapper.map(lvd->type()), value,
			m_scope.bindings.blobAggregates.get(lvd->id()),
			static_cast<int>(awst::NameGen::next("SolAssignment.respill")),
			m_loc, m_ctx.preEffects()))
		throw std::runtime_error("Cannot rebind blob-backed memory assignment");
	return value;
}

std::optional<std::shared_ptr<awst::Expression>>
SolAssignment::tryHandleScratchReferenceSlotWrite()
{
	if (!m_ctx.typeMapper.profile().scratchMemoryModel
		|| m_assignment.assignmentOperator() != Token::Assign)
		return std::nullopt;
	auto const& lhs = m_assignment.leftHandSide();
	if (!dynamic_cast<IndexAccess const*>(&lhs) && !dynamic_cast<MemberAccess const*>(&lhs))
		return std::nullopt;
	auto const* reference = dynamic_cast<ReferenceType const*>(lhs.annotation().type);
	if (!reference || reference->location() != DataLocation::Memory)
		return std::nullopt;
	auto slot = SolIndexAccess::resolveBlobOffset(m_ctx, m_scope, lhs, m_loc, /*_derefLeaf=*/false);
	if (!slot)
		return std::nullopt;
	slot = m_ctx.emitSequencedOperand({}, std::move(slot), true, m_loc);
	auto const& rhs = m_assignment.rightHandSide();
	auto const* wtype = m_ctx.typeMapper.map(reference);
	std::shared_ptr<awst::Expression> pointer;
	if (auto existing = SolIndexAccess::resolveBlobReference(m_ctx, m_scope, rhs, m_loc))
		pointer = m_ctx.emitSequencedOperand(
			std::move(existing->effects), std::move(existing->value), true, m_loc);
	else
	{
		auto lowered = m_ctx.lower(rhs, false);
		auto value = m_ctx.emitSequencedOperand(
			std::move(lowered.effects), std::move(lowered.value), true, m_loc);
		value = ConversionPlan{rhs.annotation().type, reference, wtype,
			ConversionPlan::Context::Assignment}.emit(std::move(value), m_loc, &m_ctx.preEffects());
		auto id = awst::NameGen::next("SolAssignment.referenceSlot");
		std::string name = "__slot_ref_" + std::to_string(id);
		if (!builder::spillEvmMemoryValue(m_ctx.typeMapper, reference, wtype,
				std::move(value), name, id, m_loc, m_ctx.preEffects()))
			throw std::runtime_error("Cannot spill a memory value for a reference-slot write");
		pointer = awst::makeVarExpression(name, awst::WType::uint64Type(), m_loc);
	}
	AssemblyBuilder::writeMemWordDirect(m_ctx.typeMapper, slot,
		awst::makeLeftPad(awst::makeItob(pointer, m_loc), 24, m_loc),
		m_loc, m_ctx.preEffects(), std::optional<unsigned>{0});
	return SolIndexAccess::readBlobValue(m_ctx, std::move(pointer), reference, m_loc);
}

std::optional<std::shared_ptr<awst::Expression>>
SolAssignment::tryHandleEvmStorageWrite()
{
	auto const& lhsExpr = m_assignment.leftHandSide();
	if (!(m_ctx.typeMapper.profile().evmStorageLayout && EvmSlotLowering::isStorageStateRef(lhsExpr))
		&& !EvmSlotLowering::isSlotHandleRef(lhsExpr, m_ctx, m_scope))
		return std::nullopt;

	if (auto result = tryEvmStoragePointerRebind(lhsExpr)) return result;
	if (auto result = tryEvmFixedArrayWrite(lhsExpr)) return result;
	if (m_assignment.assignmentOperator() != Token::Assign)
		throw std::runtime_error("Unsupported aggregate compound assignment");
	auto const& rhsExpr = m_assignment.rightHandSide();
	auto rhs = m_ctx.lowerOperand([&] {
		std::shared_ptr<awst::Expression> value;
		if (EvmSlotLowering::isStorageStateRef(rhsExpr))
		{
			EvmSlotLowering low(m_ctx, m_scope, m_loc);
			auto address = low.resolve(rhsExpr);
			if (!address) throw std::runtime_error("Cannot resolve storage assignment source");
			value = low.readAny(*address, rhsExpr.annotation().type);
		}
		else value = buildExpr(rhsExpr);
		return ConversionPlan{rhsExpr.annotation().type, lhsExpr.annotation().type,
			m_ctx.typeMapper.map(lhsExpr.annotation().type), ConversionPlan::Context::Assignment}.emit(
				std::move(value), m_loc, &m_ctx.preEffects());
	}, false);
	auto value = m_ctx.emitSequencedOperand(std::move(rhs.effects), std::move(rhs.value), true, m_loc);
	return ResolvedLValue(m_ctx, lhsExpr, m_loc).write(std::move(value));
}

std::optional<std::shared_ptr<awst::Expression>>
SolAssignment::tryEvmStoragePointerRebind(Expression const& _lhs)
{
	// Storage POINTER rebind: `ptr = <storage ref>` on a storage local
	// re-points the biguint slot handle (runtime value — safe in conditionals,
	// unlike the compile-time alias rebinding of the named-cell model).
	if (m_assignment.assignmentOperator() != Token::Assign)
		return {};
	auto const* lid = dynamic_cast<Identifier const*>(&_lhs);
	if (!lid)
		return {};
	auto const* lvd = dynamic_cast<VariableDeclaration const*>(
		lid->annotation().referencedDeclaration);
	if (!lvd || lvd->isStateVariable() || !lvd->isLocalVariable()
		|| lvd->referenceLocation() != VariableDeclaration::Location::Storage)
		return {};
	auto const* rhsT = m_assignment.rightHandSide().annotation().type;
	bool rhsStorage = rhsT
		&& (dynamic_cast<MappingType const*>(rhsT)
			|| (dynamic_cast<ReferenceType const*>(rhsT)
				&& rhsT->dataStoredIn(DataLocation::Storage)));
	if (!rhsStorage)
		return {};
	EvmSlotLowering low(m_ctx, m_scope, m_loc);
	auto r = low.resolve(m_assignment.rightHandSide());
	auto l = low.resolve(_lhs);
	if (!r || !l)
		throw std::runtime_error("Cannot resolve storage pointer rebind");
	// PRE-pending, and the expression VALUE is the pointer:
	// `(m = m2)[2] = 21` indexes the assignment's value —
	// makeZero here sent the write to SLOT 0 (= the first
	// mapping!), and a post-queued rebind ran after it.
	m_ctx.preEffects().push_back(
		awst::makeAssignmentStatement(
			l->slot,
			r->slot, m_loc));
	return l->slot;
}

std::optional<std::shared_ptr<awst::Expression>>
SolAssignment::tryEvmFixedArrayWrite(Expression const& lhs)
{
	auto const* target = dynamic_cast<ArrayType const*>(lhs.annotation().type);
	auto const& rhs = m_assignment.rightHandSide();
	auto const* source = dynamic_cast<ArrayType const*>(rhs.annotation().type);
	if (m_assignment.assignmentOperator() != Token::Assign || !target || !source
		|| target->isDynamicallySized() || source->isDynamicallySized()
		|| target->isByteArrayOrString() || !EvmSlotLowering::isStorageStateRef(rhs))
		return std::nullopt;
	EvmSlotLowering low(m_ctx, m_scope, m_loc);
	auto from = low.resolve(rhs);
	if (!from) throw std::runtime_error("Cannot resolve fixed-array assignment source");
	from->slot = m_ctx.emitSequencedOperand({}, from->slot, true, m_loc);
	auto to = low.resolve(lhs);
	if (!to) throw std::runtime_error("Cannot resolve fixed-array assignment destination");
	to->slot = m_ctx.emitSequencedOperand({}, to->slot, true, m_loc);
	auto copy = m_ctx.lowerOperand([&] {
		if (!target->baseType()->isValueType()
			|| target->baseType()->identifier() != source->baseType()->identifier()
			|| m_ctx.typeMapper.map(target->baseType()) == awst::WType::accountType())
			return emitEvmConvertingArrayCopy(low, target, source, to->slot, from->slot);
		auto result = trySlotBasedArrayWrite(Token::Assign, to->slot, from->slot);
		if (!result || !*result) throw std::runtime_error("Cannot copy fixed storage array");
		return *result;
	}, false);
	// solc makes self-copy a no-op, including dirty padding and dynamic tails.
	auto body = awst::makeBlock(m_loc);
	for (auto& statement: copy.effects.pre) body->body.push_back(std::move(statement));
	for (auto& statement: copy.effects.post) body->body.push_back(std::move(statement));
	m_ctx.preEffects().push_back(awst::makeIfElse(
		awst::makeNumericCompare(to->slot, awst::NumericComparison::Ne, from->slot, m_loc),
		std::move(body), nullptr, m_loc));
	// Assignment of a reference type yields the destination reference (solc),
	// after all writes complete; consumers materialize it at their value boundary.
	return to->slot;
}

std::shared_ptr<awst::Expression> SolAssignment::emitEvmConvertingArrayCopy(
	EvmSlotLowering& low, ArrayType const* target, ArrayType const* source,
	std::shared_ptr<awst::Expression> const& to,
	std::shared_ptr<awst::Expression> const& from)
{
	if (target->length() > 64) throw SizeError("converting array copy exceeds the 64-element unroll capacity");
	for (unsigned i = 0; i < target->length(); ++i)
	{
		auto index = awst::makeIntegerConstant(i, m_loc, awst::WType::biguintType());
		auto destination = low.elemAddr(to, index, target->baseType());
		auto value = TypeCoercion::makeDefaultValue(destination.wtype, m_loc);
		if (i < source->length())
		{
			auto origin = low.elemAddr(from, index, source->baseType());
			value = ConversionPlan{source->baseType(), target->baseType(), destination.wtype,
				ConversionPlan::Context::Assignment}.emit(
					low.readAny(origin, source->baseType()), m_loc, &m_ctx.preEffects());
		}
		if (!low.writeAny(destination, target->baseType(), value, m_ctx.preEffects()))
			throw std::runtime_error("Cannot write converted storage-array element");
	}
	return to;
}

std::optional<std::shared_ptr<awst::Expression>>
SolAssignment::trySlotBasedArrayWrite(
	Token _op,
	std::shared_ptr<awst::Expression> const& _target,
	std::shared_ptr<awst::Expression> const& _value)
{
	if (_op != Token::Assign || _target->wtype != awst::WType::biguintType())
		return std::nullopt;
	auto const* target = dynamic_cast<ArrayType const*>(
		m_assignment.leftHandSide().annotation().type);
	auto const* source = dynamic_cast<ArrayType const*>(
		m_assignment.rightHandSide().annotation().type);
	if (!target) target = source;
	if (!target || target->isDynamicallySized()) return std::nullopt;

	EvmSlotLowering low(m_ctx, m_scope, m_loc);
	std::vector<std::shared_ptr<awst::Statement>> out;
	if (_value->wtype == awst::WType::biguintType())
	{
		if (!source || source->isDynamicallySized())
			throw SizeError("fixed storage copy requires a fixed source array");
		if (!target->baseType()->isValueType()
			|| target->baseType()->identifier() != source->baseType()->identifier()
			|| m_ctx.typeMapper.map(target->baseType()) == awst::WType::accountType())
			return emitEvmConvertingArrayCopy(low, target, source, _target, _value);

		// solc copyValueArrayToStorageFunction: same-type scalar arrays copy
		// words, masking every word's unused bytes and the final partial word.
		auto slots = target->storageSize();
		if (slots > 256) throw SizeError("fixed storage copy exceeds the 256-slot unroll capacity");
		auto layout = builder::SlotHandleAccess::layoutFor(target->baseType());
		for (unsigned j = 0; j < slots; ++j)
		{
			auto slot = [&](std::shared_ptr<awst::Expression> base) {
				return awst::makeBigUIntBinOp(std::move(base), awst::BigUIntBinaryOperator::Add,
					awst::makeIntegerConstant(j, m_loc, awst::WType::biguintType()), m_loc);
			};
			std::shared_ptr<awst::Expression> word = awst::makeZero(m_loc, awst::WType::biguintType());
			if (j < source->storageSize())
			{
				word = builder::SlotHandleAccess::readSlot(slot(_value), m_loc);
				auto remaining = source->length() - solidity::u256(j) * layout.perSlot;
				unsigned count = remaining < layout.perSlot
					? static_cast<unsigned>(remaining) : layout.perSlot;
				unsigned width = count * layout.size;
				if (width < 32)
					word = awst::makeAsBiguint(awst::makeLeftPadToN(
						awst::makeAsBytes(std::move(word), m_loc), width, m_loc), m_loc);
			}
			out.push_back(builder::SlotHandleAccess::writeSlot(slot(_target), std::move(word), m_loc));
		}
	}
	else
	{
		// Value-to-storage uses the same recursive writer as initializers and
		// ordinary aggregate assignment, including struct padding and tails.
		EvmSlotLowering::Addr address;
		address.slot = _target;
		address.solType = target;
		auto value = ConversionPlan{m_assignment.rightHandSide().annotation().type,
			target, m_ctx.typeMapper.map(target), ConversionPlan::Context::Assignment}.emit(_value, m_loc, &out);
		if (!low.writeArrayValue(address, target, std::move(value), out))
			throw SizeError("cannot write fixed storage array");
	}
	for (auto& statement: out) m_ctx.queuePostEffect(std::move(statement));
	return awst::makeZero(m_loc, awst::WType::biguintType());
}

std::optional<std::shared_ptr<awst::Expression>>
SolAssignment::trySlotBasedScalarWrite(
	Token _op,
	std::shared_ptr<awst::Expression> const& _target,
	std::shared_ptr<awst::Expression>& _value)
{
	// Scalar slot-based write: the target is already a full-width slot.
	if (!dynamic_cast<awst::BigUIntBinaryOperation const*>(_target.get())
		|| _target->wtype != awst::WType::biguintType())
		return std::nullopt;

	// Compound: read current first, apply op.
	if (_op != Token::Assign)
	{
		auto readCall = awst::makeSubroutineCall(
			awst::SubroutineID{"__puyasol___storage_read"}, awst::WType::biguintType(), m_loc);
		awst::pushCallArg(readCall->args, "__slot", _target);

		auto* targetSolType = m_assignment.leftHandSide().annotation().type;
		_value = widenSignedCompoundRhs(std::move(_value));
		_value = eb::AssignmentHelper::computeCompoundOrFallback(
			m_ctx, _op, _op, targetSolType, std::move(readCall),
			std::move(_value), _target->wtype, m_loc);
	}

	auto call = awst::makeSubroutineCall(
		awst::SubroutineID{"__puyasol___storage_write"}, awst::WType::voidType(), m_loc);
	awst::pushCallArg(call->args, "__slot", _target);
	awst::pushCallArg(call->args, "__value", std::move(_value));
	m_ctx.queuePostExpression(std::move(call), m_loc);
	return std::shared_ptr<awst::Expression>{awst::makeZero(m_loc, awst::WType::biguintType())};
}

std::optional<std::shared_ptr<awst::Expression>>
SolAssignment::tryTupleAssignment(
	std::shared_ptr<awst::Expression>& _target,
	std::shared_ptr<awst::Expression>& _value)
{
	if (!dynamic_cast<awst::TupleExpression const*>(_target.get())) return std::nullopt;
	auto const* sourceLhs = dynamic_cast<solidity::frontend::TupleExpression const*>(
		&m_assignment.leftHandSide());
	return handleTupleAssignment(std::move(_target), std::move(_value), sourceLhs);
}

std::shared_ptr<awst::Expression>
SolAssignment::widenSignedCompoundRhs(std::shared_ptr<awst::Expression> _value)
{
	// Solidity `a op= b` is `a = a op T(b)`: the RHS converts to the TARGET
	// type FIRST. A narrower SIGNED rhs must reach the compound compute in
	// the target's CANONICAL form — tryComputeCompoundValue builds both
	// operand builders at the TARGET type, so the signed-div/mod path
	// sign-extends from the target width and a not-yet-widened negative
	// divisor read as huge-positive (`int128 x; int16 y=-32768; x /= y`
	// divided by +1.8e19). uint64-carried rhs into a biguint-backed target
	// needs promotion + extension to 256-bit TC; same-carrier widens go
	// through signExtendSignedWiden. Shared by every compound site
	// (applyCompoundAssignment, transient, slot-scalar) so they can't drift.
	auto const* rhsSolType = m_assignment.rightHandSide().annotation().type;
	auto const* tgtSolType = m_assignment.leftHandSide().annotation().type;
	auto rhsInt = builder::SolIntType::fromSol(rhsSolType);
	auto tgtInt = builder::SolIntType::fromSol(tgtSolType);
	if (!_value || !rhsInt || !tgtInt || !rhsInt->isSigned || !tgtInt->isSigned
		|| rhsInt->bits >= tgtInt->bits)
		return _value;
	if (tgtInt->bits > 64 && _value->wtype == awst::WType::uint64Type())
		return builder::TypeCoercion::signExtendToUint256(
			builder::TypeCoercion::implicitNumericCast(
				std::move(_value), awst::WType::biguintType(), m_loc),
			rhsInt->bits, m_loc);
	return builder::TypeCoercion::signExtendSignedWiden(
		std::move(_value), rhsSolType, tgtSolType, m_loc);
}

} // namespace puyasol::builder::sol_ast
