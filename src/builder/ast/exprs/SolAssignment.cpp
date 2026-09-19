/// @file SolAssignment.cpp
/// Top-level assignment translator (try*/apply* pipeline).
/// Shape-specific handlers live in sibling SolAssignment*.cpp.

#include <algorithm>
#include "builder/ast/exprs/SolAssignment.h"
#include "builder/ast/exprs/SolTupleExpression.h"
#include "builder/eb/ResolvedLValue.h"
#include "builder/eb/AssemblyBoundary.h"
#include "builder/eb/CalldataReference.h"
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

SolAssignment::SolAssignment(eb::ContractContext& _ctx, Assignment const& _node, bool _resultUsed)
	: SolExpression(_ctx, _node), m_assignment(_node), m_resultUsed(_resultUsed)
{
}

// toAwst pipeline:
//   (1) Pre-buildExpr early-outs (transient, storage-ptr, multi-box, push-assign)
//   (2) Build target + value
//   (3) Store through the shared ResolvedLValue implementation.
std::shared_ptr<awst::Expression> SolAssignment::toAwst()
{
	Token op = m_assignment.assignmentOperator();
	if (op == Token::Assign)
		if (auto const* id = SolcFacts::expressionAs<Identifier>(&m_assignment.leftHandSide()))
			if (auto const* declaration = dynamic_cast<VariableDeclaration const*>(id->annotation().referencedDeclaration))
				if (auto copy = assemblyScalarCopy(m_ctx, *declaration, m_assignment.rightHandSide(), m_loc))
					return ResolvedLValue(m_ctx, m_assignment.leftHandSide(), m_loc).write(std::move(copy));
	if (auto const* id = SolcFacts::expressionAs<Identifier>(&m_assignment.leftHandSide()))
		if (auto const* declaration = dynamic_cast<VariableDeclaration const*>(id->annotation().referencedDeclaration);
			declaration && declaration->referenceLocation() == VariableDeclaration::Location::CallData)
			if (auto reference = CalldataReference::resolve(m_ctx, m_assignment.rightHandSide(), m_loc))
			{
				reference->bind(m_ctx, *declaration, m_loc);
				return m_resultUsed ? reference->read(m_ctx, m_loc) : awst::makeVoidConstant(m_loc);
			}
			else if (m_scope.function && m_scope.function->hasAssemblyCalldata)
				throw SizeError("calldata reference assignment has no preserved input coordinates");

	// (1) Pre-buildExpr early-outs.
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
	auto const* sourceLhs = SolcFacts::expressionAs<TupleExpression>(&m_assignment.leftHandSide());
	if (sourceLhs && dynamic_cast<TupleType const*>(sourceLhs->annotation().type))
	{
		std::vector<VariableDeclaration const*> bindings;
		for (auto const& component: sourceLhs->components())
		{
			auto const* id = component ? SolcFacts::expressionAs<Identifier>(&SolcFacts::functionExpression(*component)) : nullptr;
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
		resolve = [&](Expression const& source) -> std::shared_ptr<awst::Expression> {
			auto const& component = SolcFacts::unparenthesized(source);
			if (auto const* nested = SolcFacts::expressionAs<TupleExpression>(&component))
			{
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
			auto const* identifier = SolcFacts::expressionAs<Identifier>(&component);
			auto const* declaration = identifier
				? dynamic_cast<VariableDeclaration const*>(identifier->annotation().referencedDeclaration) : nullptr;
			if (declaration)
				if (auto reference = CalldataReference::local(m_scope, *declaration, m_loc)) return reference->pack(m_loc);
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
		if (auto const* lid = SolcFacts::expressionAs<Identifier>(&m_assignment.leftHandSide()))
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
	return emitGenericAssignment(op, std::move(target), std::move(value));
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
	auto const* lhsCall = SolcFacts::expressionAs<FunctionCall>(&m_assignment.leftHandSide());
	if (!lhsCall || !lhsCall->arguments().empty()) return std::nullopt;
	auto const* member = SolcFacts::expressionAs<MemberAccess>(&SolcFacts::functionExpression(lhsCall->expression()));
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
		&& !SolcFacts::expressionAs<Identifier>(&SolcFacts::functionExpression(lhs));
	if ((!type->isValueType() && !memoryLeaf && !resolution.isBoxedAggregate())
		|| !resolution.isAddressed()) return std::nullopt;
	if (memoryLeaf && !type->isValueType() && resolution.isMemory())
		if (auto reference = SolIndexAccess::resolveBlobReference(
			m_ctx, m_scope, m_assignment.rightHandSide(), m_loc))
		{
			auto offset = m_ctx.emitSequencedOperand(std::move(reference->effects),
				std::move(reference->value), true, m_loc);
			ResolvedLValue target(m_ctx, lhs, m_loc, std::move(resolution));
			offset = target.writeMemoryReference(std::move(offset));
			return m_resultUsed ? SolIndexAccess::readBlobValue(m_ctx, std::move(offset), type, m_loc) : offset;
		}
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
	if (_op != Token::Assign) return _value;
	auto const* lhsType = m_assignment.leftHandSide().annotation().type;
	return TypeCoercion::checkedEnum(std::move(_value), lhsType, m_loc, &m_ctx.preEffects());
}

std::optional<std::shared_ptr<awst::Expression>>
SolAssignment::tryHandleBlobRespill()
{
	if (m_assignment.assignmentOperator() != Token::Assign)
		return std::nullopt;
	auto const* lid = SolcFacts::expressionAs<Identifier>(&m_assignment.leftHandSide());
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
		return m_resultUsed ? SolIndexAccess::readBlobValue(m_ctx, std::move(offset), lvd->type(), m_loc) : offset;
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
	auto const* lid = SolcFacts::expressionAs<Identifier>(&_lhs);
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
		emitEvmScalarArrayCopy(*target, *source, to->slot, from->slot);
		return to->slot;
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

void SolAssignment::emitEvmScalarArrayCopy(
	ArrayType const& target, ArrayType const& source,
	std::shared_ptr<awst::Expression> const& to,
	std::shared_ptr<awst::Expression> const& from)
{
	// solc copyValueArrayToStorageFunction: same-type scalar arrays copy
	// words, masking every word's unused bytes and the final partial word.
	auto slots = target.storageSize();
	if (slots > 256) throw SizeError("fixed storage copy exceeds the 256-slot unroll capacity");
	auto layout = builder::SlotHandleAccess::layoutFor(target.baseType());
	for (unsigned j = 0; j < slots; ++j)
	{
		auto slot = [&](std::shared_ptr<awst::Expression> base) {
			return awst::makeBigUIntBinOp(std::move(base), awst::BigUIntBinaryOperator::Add,
				awst::makeIntegerConstant(j, m_loc, awst::WType::biguintType()), m_loc);
		};
		std::shared_ptr<awst::Expression> word = awst::makeZero(m_loc, awst::WType::biguintType());
		if (j < source.storageSize())
		{
			word = builder::SlotHandleAccess::readSlot(slot(from), m_loc);
			auto remaining = source.length() - solidity::u256(j) * layout.perSlot;
			unsigned count = remaining < layout.perSlot
				? static_cast<unsigned>(remaining) : layout.perSlot;
			unsigned width = count * layout.size;
			if (width < 32)
				word = awst::makeAsBiguint(awst::makeLeftPadToN(
					awst::makeAsBytes(std::move(word), m_loc), width, m_loc), m_loc);
		}
		m_ctx.queuePostEffect(builder::SlotHandleAccess::writeSlot(slot(to), std::move(word), m_loc));
	}
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
			builder::TypeCoercion::coerceScalar(
				std::move(_value), awst::WType::biguintType(), m_loc),
			rhsInt->bits, m_loc);
	return builder::TypeCoercion::signExtendSignedWiden(
		std::move(_value), rhsSolType, tgtSolType, m_loc);
}

} // namespace puyasol::builder::sol_ast
