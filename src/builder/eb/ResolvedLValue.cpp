#include "builder/eb/ResolvedLValue.h"
#include "builder/eb/AssemblyBoundary.h"
#include "builder/context/TranslationContext.h"
#include "builder/solc/SolcFacts.h"
#include "builder/storage/StoragePlace.hpp"
#include "builder/solc/StorageRefPointer.h"
#include "builder/ast/exprs/SolIndexAccess.h"
#include "builder/ast/calls/SolInternalCall.h"
#include "builder/eb/AssignmentHelper.h"
#include "builder/yul/AssemblyBuilder.h"
#include "builder/codec/EvmMemoryCodec.h"
#include "builder/codec/EvmValueCodec.h"
#include "builder/storage/StorageMapper.h"
#include "builder/target/EvmLayoutMode.h"
#include "awst/NameGen.h"
#include "builder/storage/TransientStorage.h"
#include "builder/codec/Arc4Defaults.h"
#include "builder/types/TypeCoercion.h"
#include "builder/types/TypeMapper.h"

#include <libsolidity/ast/AST.h>
#include <algorithm>
#include <stdexcept>

namespace puyasol::builder::sol_ast
{
using namespace solidity::frontend;
using Expr = ResolvedLValue::Expr;

namespace
{
bool slotDestination(eb::ContractContext& ctx, Expression const& source)
{
	return (ctx.typeMapper.profile().evmStorageLayout && EvmSlotLowering::isStorageStateRef(source))
		|| EvmSlotLowering::isSlotHandleRef(source, ctx, ctx.scope());
}

VariableDeclaration const* transientDeclaration(eb::ContractContext& ctx, Expression const& source)
{
	auto const* id = SolcFacts::expressionAs<Identifier>(&source);
	auto const* decl = id ? dynamic_cast<VariableDeclaration const*>(id->annotation().referencedDeclaration) : nullptr;
	return decl && ctx.transientStorage && ctx.transientStorage->isTransient(*decl) ? decl : nullptr;
}

bool blobRoot(eb::ContractContext& ctx, Expression const& source)
{
	for (auto const* root: SolcFacts::referenceSources(source))
	{
		if (auto const* call = SolcFacts::expressionAs<FunctionCall>(root);
			call && call->annotation().type->dataStoredIn(DataLocation::Memory)
			&& SolInternalCall::hasReferenceReturns(*call)) return true;
		if (auto const* id = SolcFacts::expressionAs<Identifier>(root))
			if (auto const* declaration = id->annotation().referencedDeclaration;
				declaration && !ctx.scope().bindings.blobAggregates.get(declaration->id()).empty())
				return true;
	}
	return false;
}
} // namespace

std::optional<ResolvedLValue::Resolution::AggregatePath> ResolvedLValue::aggregatePath(eb::ContractContext& ctx, Expression const& source)
{
	if (ctx.typeMapper.profile().evmStorageLayout) return std::nullopt;
	std::vector<Expression const*> steps;
	auto const* cursor = &source;
	for (;;)
	{
		cursor = &SolcFacts::functionExpression(*cursor);
		if (auto const* index = SolcFacts::expressionAs<IndexAccess>(cursor))
		{
			if (!index->indexExpression()
				|| dynamic_cast<MappingType const*>(index->baseExpression().annotation().type))
				return std::nullopt;
			steps.push_back(cursor);
			cursor = &index->baseExpression();
		}
		else if (auto const* member = SolcFacts::expressionAs<MemberAccess>(cursor);
			member && dynamic_cast<StructType const*>(member->expression().annotation().type))
		{
			if (!transparentMappingWrapper(member->expression().annotation().type))
				steps.push_back(cursor);
			cursor = &member->expression();
		}
		else break;
	}
	auto const* identifier = SolcFacts::expressionAs<Identifier>(cursor);
	auto const* declaration = identifier
		? dynamic_cast<VariableDeclaration const*>(identifier->annotation().referencedDeclaration) : nullptr;
	if (!declaration || steps.empty() || declaration->isConstant() || declaration->immutable())
		return std::nullopt;
	auto const* array = dynamic_cast<ArrayType const*>(declaration->type());
	if ((!array && !dynamic_cast<StructType const*>(declaration->type()))
		|| (array && array->isByteArrayOrString())) return std::nullopt;
	auto key = ctx.scope().bindings.mappingKeyParams.get(declaration->id());
	auto offset = ctx.scope().bindings.structRefOffsets.get(declaration->id());
	// A byte-window snapshot needs a fixed serialized extent. Storage-only
	// holders (e.g. nested mappings) retain their own reference diagnostics.
	if (!offset.empty() && !computeEncodedElementSize(ctx.typeMapper.map(declaration->type()))
		.fixedBytes<uint64_t>().value_or(0)) return std::nullopt;
	auto binding = declaration->isStateVariable()
		? ctx.storageMapper.physicalBindingFor(*declaration) : StorageMapper::PhysicalBinding{};
	bool const direct = declaration->isStateVariable() && binding.kind == awst::AppStorageKind::Box;
	bool const paged = direct && StorageMapper::isMultiBoxArray(ctx.typeMapper.map(declaration->type()));
	// Direct single-box arrays already support bounded element reads/writes.
	if ((!direct && key.empty()) || (direct && array && !paged)) return std::nullopt;
	std::reverse(steps.begin(), steps.end());
	return Resolution::AggregatePath{declaration, std::move(steps), std::move(key), std::move(offset),
		std::move(binding), paged};
}

namespace
{
Expr readable(Expr value, awst::SourceLocation const& loc)
{
	if (dynamic_cast<awst::StateGet const*>(value.get())) return value;
	if (auto base = StoragePlace::projectionBase(value))
		return StoragePlace::withProjectionBase(value, readable(std::move(base), loc));
	if (dynamic_cast<awst::BoxValueExpression const*>(value.get()))
		return StorageMapper::makeStateGetWithDefault(value, value->wtype, loc);
	return value;
}
}

ResolvedLValue::Resolution ResolvedLValue::classify(
	eb::ContractContext& ctx, Expression const& source, bool alreadyBuilt)
{
	if (auto const* declaration = transientDeclaration(ctx, source)) return Resolution{declaration};
	if (alreadyBuilt) return {};
	if (slotDestination(ctx, source)) return Resolution{Resolution::Slot{}};
	if (blobRoot(ctx, source)) return Resolution{Resolution::Blob{}};
	if (auto path = aggregatePath(ctx, source)) return Resolution{std::move(*path)};
	return {};
}

Expr ResolvedLValue::target() const
{
	if (auto const* plain = std::get_if<Target>(&m_destination)) return plain->value;
	if (auto const* aggregate = std::get_if<Aggregate>(&m_destination)) return aggregate->target;
	return nullptr;
}

Expr ResolvedLValue::pin(Expr value)
{
	return m_ctx.emitSequencedOperand({}, std::move(value), true, m_loc);
}

ResolvedLValue::ResolvedLValue(eb::ContractContext& ctx, Expression const& source,
	awst::SourceLocation const& loc, Expr built)
	: ResolvedLValue(ctx, source, loc, classify(ctx, source, bool(built)), built)
{}

ResolvedLValue::ResolvedLValue(eb::ContractContext& ctx, Expression const& source,
	awst::SourceLocation const& loc, Resolution resolution, Expr built)
	: m_ctx(ctx), m_type(source.annotation().type), m_native(ctx.typeMapper.map(m_type)), m_loc(loc)
{
	if (auto const* identifier = SolcFacts::expressionAs<Identifier>(&source))
		if (auto const* declaration = dynamic_cast<VariableDeclaration const*>(identifier->annotation().referencedDeclaration);
			declaration && ctx.scope().bindings.assemblyWords.find(declaration->id()))
		{
			m_destination = AssemblyWord{declaration};
			return;
		}
	if (auto const* declaration = std::get_if<VariableDeclaration const*>(&resolution.m_kind))
	{
		m_destination = Transient{*declaration};
		return;
	}
	auto const* index = SolcFacts::expressionAs<IndexAccess>(&SolcFacts::functionExpression(source));
	bool const bytesElement = index && EvmSlotLowering::isBytesLike(index->baseExpression().annotation().type);
	if (std::holds_alternative<Resolution::Slot>(resolution.m_kind))
	{
		EvmSlotLowering low(ctx, ctx.scope(), loc);
		auto address = low.resolve(bytesElement ? index->baseExpression() : source);
		if (!address) throw std::runtime_error("Cannot resolve storage assignment destination");
		Slot slot{std::move(*address), nullptr};
		slot.address.slot = pin(slot.address.slot);
		if (slot.address.byteOffset) slot.address.byteOffset = pin(slot.address.byteOffset);
		if (bytesElement)
		{
			slot.byteIndex = pin(TypeCoercion::checkedIndexToUint64(ctx.preEffects(),
				ctx.pinIfWriteBacks(ctx.lower(*index->indexExpression(), false), loc), loc));
			ctx.queuePreExpression(awst::makeAssert(awst::makeNumericCompare(slot.byteIndex,
				awst::NumericComparison::Lt, awst::makeLen(low.readBytesValue(slot.address), loc), loc),
				loc, "bytes index out of range"), loc);
		}
		m_destination = std::move(slot);
		return;
	}
	if (std::holds_alternative<Resolution::Blob>(resolution.m_kind))
	{
		bool const referenceSlot = !m_type->isValueType()
			&& !SolcFacts::expressionAs<Identifier>(&source);
		auto offset = SolIndexAccess::resolveBlobOffset(ctx, ctx.scope(), source, loc, !referenceSlot);
		if (m_type->isValueType() || referenceSlot) offset = pin(std::move(offset));
		if (!offset) throw std::runtime_error("Cannot resolve memory assignment destination");
		m_destination = Blob{std::move(offset), bytesElement, referenceSlot};
		return;
	}
	if (auto const* path = std::get_if<Resolution::AggregatePath>(&resolution.m_kind))
	{
		m_destination = resolveAggregate(*path);
		return;
	}
	if (!built)
	{
		auto lowered = ctx.lower(source, false);
		built = ctx.emitSequencedOperand(std::move(lowered.effects), std::move(lowered.value), false, loc);
	}
	built = freezeTarget(ctx, std::move(built), loc);
	if (!built) throw std::runtime_error("Cannot resolve assignment destination");
	m_destination = Target{std::move(built)};
}

ResolvedLValue::Aggregate ResolvedLValue::resolveAggregate(Resolution::AggregatePath const& path)
{
	Aggregate aggregate;
	auto const* rootType = m_ctx.typeMapper.map(path.declaration->type());
	size_t first = 0;
	if (path.paged)
	{
		auto const* index = SolcFacts::expressionAs<IndexAccess>(path.steps.front());
		if (!index) throw std::logic_error("Paged aggregate path does not begin with an index");
		auto value = m_ctx.pinIfWriteBacks(m_ctx.lower(*index->indexExpression(), false), m_loc);
		auto page = StorageMapper::arrayPageForIndex(path.binding.key, rootType,
			std::move(value), m_ctx.preEffects(), m_loc);
		aggregate.key = pin(page.key);
		aggregate.offset = pin(page.offset);
		aggregate.ensure = StorageMapper::ensureArrayPage(page, m_loc);
		rootType = page.elementType;
		if (computeEncodedElementSize(rootType).fixedBytes<unsigned>().value_or(0)
			> StorageMapper::kAvmStackValueMax)
			throw SizeError("multi-box element exceeds the AVM stack-value limit");
		first = 1;
	}
	else
	{
		aggregate.key = path.key.empty()
			? awst::makeUtf8BytesConstant(path.binding.key, m_loc, awst::WType::boxKeyType())
			: pin(awst::makeReinterpretCast(awst::makeVarExpression(path.key,
				awst::WType::bytesType(), m_loc), awst::WType::boxKeyType(), m_loc));
		if (!path.offset.empty())
			aggregate.offset = pin(awst::makeVarExpression(path.offset, awst::WType::uint64Type(), m_loc));
	}
	if (aggregate.offset)
		aggregate.initial = StorageMapper::makeBoxWindowRead(
			m_ctx.typeMapper, aggregate.key, aggregate.offset, rootType, m_loc);
	else
	{
		auto box = awst::makeBoxValueExpression(aggregate.key, rootType, m_loc);
		box->preserveEmptyBox = path.key.empty() && path.binding.preservesEmptyBox();
		aggregate.box = box;
		aggregate.initial = StorageMapper::makeStateGetWithDefault(box, rootType, m_loc);
	}
	aggregate.root = awst::makeVarExpression("__aggregate_"
		+ std::to_string(awst::NameGen::next("ResolvedLValue.aggregate")), rootType, m_loc);
	aggregate.target = aggregate.root;
	for (size_t i = first; i < path.steps.size(); ++i)
	{
		if (auto const* index = SolcFacts::expressionAs<IndexAccess>(path.steps[i]))
		{
			auto value = m_ctx.pinIfWriteBacks(m_ctx.lower(*index->indexExpression(), false), m_loc);
			value = pin(TypeCoercion::checkedIndexToUint64(m_ctx.preEffects(), std::move(value), m_loc));
			auto const* element = awst::arrayElementType(aggregate.target->wtype);
			if (!element) throw std::logic_error("Aggregate destination is not an array");
			aggregate.target = awst::makeIndexExpression(aggregate.target, std::move(value), element, m_loc);
		}
		else
		{
			auto const& member = dynamic_cast<MemberAccess const&>(*path.steps[i]);
			auto const* field = awst::structFieldType(aggregate.target->wtype, member.memberName());
			if (!field) throw std::logic_error("Aggregate destination has no field");
			aggregate.target = awst::makeFieldExpression(aggregate.target, member.memberName(), field, m_loc);
		}
	}
	return aggregate;
}

void ResolvedLValue::loadAggregate()
{
	if (auto const* aggregate = std::get_if<Aggregate>(&m_destination))
		// Only the address is frozen. Re-read the container at each store so
		// intervening calls/tuple stores cannot lose sibling-field mutations.
		m_ctx.queuePreEffect(awst::makeAssignmentStatement(
			aggregate->root, aggregate->initial, m_loc));
}

Expr ResolvedLValue::freezeTarget(eb::ContractContext& ctx, Expr target, awst::SourceLocation const& loc)
{
	// A later tuple store can change an index/key variable. Freeze addresses,
	// not container contents, while preserving the original assignable path.
	if (auto const* index = dynamic_cast<awst::IndexExpression const*>(target.get()))
	{
		auto base = freezeTarget(ctx, index->base, loc);
		auto offset = ctx.emitSequencedOperand({}, index->index, true, loc);
		return awst::makeIndexExpression(std::move(base), std::move(offset), index->wtype, loc);
	}
	if (auto base = StoragePlace::projectionBase(target))
		return StoragePlace::withProjectionBase(target, freezeTarget(ctx, std::move(base), loc));
	if (dynamic_cast<awst::BoxValueExpression const*>(target.get())
		|| dynamic_cast<awst::AppStateExpression const*>(target.get()))
	{
		auto place = StoragePlace::fromRead(target);
		return place->makeField(ctx.emitSequencedOperand({}, place->key, true, loc), loc);
	}
	return target;
}

Expr ResolvedLValue::read()
{
	if (auto const* word = std::get_if<AssemblyWord>(&m_destination))
		return readAssemblyScalar(m_ctx.scope(), m_ctx.typeMapper, *word->declaration, m_loc, m_ctx.preEffects());
	if (auto const* transient = std::get_if<Transient>(&m_destination))
		return m_ctx.transientStorage->buildRead(*transient->declaration, m_loc);
	if (auto const* slot = std::get_if<Slot>(&m_destination))
	{
		EvmSlotLowering low(m_ctx, m_ctx.scope(), m_loc);
		if (slot->byteIndex) return awst::makeExtract3(low.readBytesValue(slot->address), slot->byteIndex,
			awst::makeOne(m_loc), m_loc, m_native);
		return low.readAny(slot->address, m_type);
	}
	if (auto const* blob = std::get_if<Blob>(&m_destination))
		return SolIndexAccess::readBlobValue(m_ctx, blob->referenceSlot
			? readEvmMemoryUint64Word(m_ctx.typeMapper, blob->offset, m_loc, m_ctx.preEffects())
			: blob->offset, m_type, m_loc);
	loadAggregate();
	auto value = StorageMapper::makePartialBoxReadWithDefault(
		m_ctx.typeMapper, readable(target(), m_loc), m_ctx.preEffects(), m_loc);
	if (auto const* index = dynamic_cast<awst::IndexExpression const*>(value.get());
		index && index->base->wtype->kind() == awst::WTypeKind::Bytes)
		value = awst::makeExtract3(index->base, index->index, awst::makeOne(m_loc), m_loc, m_native);
	return codec::valueFromArc4(m_ctx.typeMapper, m_type, std::move(value), m_loc);
}

void ResolvedLValue::writeTarget(Expr target, Expr value)
{
	// A bytes view can nest casts, StateGet and ARC4Decode around a struct field.
	for (;;)
	{
		if (auto const* cast = dynamic_cast<awst::ReinterpretCast const*>(target.get())) target = cast->expr;
		else if (auto const* get = dynamic_cast<awst::StateGet const*>(target.get())) target = get->field;
		else if (auto const* decode = dynamic_cast<awst::ARC4Decode const*>(target.get())) target = decode->value;
		else break;
	}
	if (auto const* index = dynamic_cast<awst::IndexExpression const*>(target.get());
		index && index->base->wtype->kind() == awst::WTypeKind::Bytes)
	{
		writeTarget(index->base, awst::makeReplace3(readable(index->base, m_loc), index->index,
			awst::makeAsBytes(std::move(value), m_loc), m_loc));
		return;
	}
	value = eb::AssignmentHelper::arc4EncodeForType(m_ctx, std::move(value), target->wtype, m_loc);
	if (auto const* field = dynamic_cast<awst::FieldExpression const*>(target.get()))
	{
		if (auto const* type = dynamic_cast<awst::ARC4Struct const*>(field->base->wtype))
		{
			auto store = eb::AssignmentHelper::buildStructFieldCowStore(m_ctx, field, type, value, m_loc);
			m_ctx.queuePreEffect(awst::makeAssignmentStatement(store.target, store.value, m_loc));
			return;
		}
		if (auto const* tuple = dynamic_cast<awst::WTuple const*>(field->base->wtype); tuple && tuple->names())
		{
			auto updated = awst::makeTupleExpression(tuple, m_loc);
			for (size_t i = 0; i < tuple->types().size(); ++i)
				updated->items.push_back((*tuple->names())[i] == field->name ? value
					: awst::makeFieldExpression(readable(field->base, m_loc), (*tuple->names())[i], tuple->types()[i], m_loc));
			writeTarget(field->base, std::move(updated));
			return;
		}
	}
	value = TypeCoercion::coerceForAssignment(std::move(value), target->wtype, m_loc, &m_ctx.preEffects());
	auto store = eb::AssignmentHelper::preparePlainStore(m_ctx, std::move(target), std::move(value), m_loc);
	m_ctx.queuePreEffect(awst::makeAssignmentStatement(store.target, store.value, m_loc));
}

Expr ResolvedLValue::write(Expr value)
{
	if (auto const* word = std::get_if<AssemblyWord>(&m_destination);
		word && isAssemblyScalarCopy(value->wtype))
	{
		m_ctx.queuePreEffect(writeAssemblyScalar(m_ctx.scope(), m_ctx.typeMapper, *word->declaration, pin(value), m_loc));
		return read();
	}
	value = pin(TypeCoercion::coerceForAssignment(std::move(value), m_native, m_loc, &m_ctx.preEffects()));
	if (auto const* word = std::get_if<AssemblyWord>(&m_destination))
		m_ctx.queuePreEffect(writeAssemblyScalar(m_ctx.scope(), m_ctx.typeMapper, *word->declaration, value, m_loc));
	else if (auto const* transient = std::get_if<Transient>(&m_destination))
		m_ctx.queuePreEffect(m_ctx.transientStorage->buildWrite(*transient->declaration, value, m_loc));
	else if (auto* slot = std::get_if<Slot>(&m_destination))
	{
		EvmSlotLowering low(m_ctx, m_ctx.scope(), m_loc);
		if (slot->byteIndex)
			low.writeBytesValue(slot->address, awst::makeReplace3(low.readBytesValue(slot->address),
				slot->byteIndex, awst::makeAsBytes(value, m_loc), m_loc), m_ctx.preEffects());
		else if (!low.writeAny(slot->address, m_type, value, m_ctx.preEffects()))
			throw std::runtime_error("Cannot write storage assignment destination");
		if (!m_type->isValueType()) return slot->address.slot;
	}
	else if (auto const* blob = std::get_if<Blob>(&m_destination))
	{
		if (blob->referenceSlot)
		{
			auto id = awst::NameGen::next("ResolvedLValue.memoryReference");
			auto name = "__memory_reference_" + std::to_string(id);
			if (!spillEvmMemoryValue(m_ctx.typeMapper, m_type, m_native, value, name, id, m_loc, m_ctx.preEffects()))
				throw std::runtime_error("Cannot allocate memory assignment destination");
			writeMemoryReference(awst::makeVarExpression(name, awst::WType::uint64Type(), m_loc));
		}
		else if (blob->packedByte)
			AssemblyBuilder::writeMemByteDirect(m_ctx.typeMapper.profile().scratchLayout, blob->offset,
				awst::makeExtract(codec::valueToEvmWord(m_ctx.typeMapper, m_type, value, m_loc), 0, 1, m_loc),
				m_loc, m_ctx.preEffects());
		else if (!writeEvmMemoryValueAt(m_ctx.typeMapper, m_type, value, blob->offset, m_loc, m_ctx.preEffects()))
			throw std::runtime_error("Cannot write memory assignment destination");
	}
	else
	{
		loadAggregate();
		auto destination = target();
		writeTarget(destination, value);
		if (auto const* aggregate = std::get_if<Aggregate>(&m_destination))
		{
			if (aggregate->ensure) m_ctx.queuePreEffect(aggregate->ensure);
			if (aggregate->offset)
				m_ctx.queuePreExpression(awst::makeBoxReplace(aggregate->key, aggregate->offset,
					awst::makeAsBytes(aggregate->root, m_loc), m_loc), m_loc);
			else
				m_ctx.queuePreEffect(awst::makeAssignmentStatement(
					aggregate->box, aggregate->root, m_loc));
		}
		if (!m_type->isValueType() && m_type->dataStoredIn(DataLocation::Storage))
			return readable(destination, m_loc);
	}
	return value;
}

Expr ResolvedLValue::writeMemoryReference(Expr offset)
{
	auto const& blob = std::get<Blob>(m_destination);
	assert(blob.referenceSlot);
	offset = pin(std::move(offset));
	AssemblyBuilder::writeMemWordDirect(m_ctx.typeMapper, blob.offset,
		awst::makeLeftPadToN(awst::makeItob(offset, m_loc), 32, m_loc), m_loc, m_ctx.preEffects());
	return offset;
}

bool ResolvedLValue::isMemoryReference() const
{
	auto const* blob = std::get_if<Blob>(&m_destination);
	return blob && blob->referenceSlot;
}

void ResolvedLValue::clear()
{
	if (auto const* blob = std::get_if<Blob>(&m_destination); blob && !m_type->isValueType())
	{
		auto offset = defaultEvmMemoryValue(m_ctx.typeMapper, m_type, m_loc, m_ctx.preEffects());
		if (blob->referenceSlot) writeMemoryReference(std::move(offset));
		else m_ctx.queuePreEffect(awst::makeAssignmentStatement(blob->offset, std::move(offset), m_loc));
		return;
	}
	if (auto const* aggregate = std::get_if<Aggregate>(&m_destination); aggregate && aggregate->ensure)
	{
		// Deleting a missing page must not allocate it. Address/bounds effects
		// were already resolved, but the COW/write group is conditional.
		auto writes = m_ctx.lowerOperand([&] {
			write(TypeCoercion::makeDefaultValue(m_native, m_loc));
			return awst::makeVoidConstant(m_loc);
		}, false);
		auto body = awst::makeBlock(m_loc);
		body->body = std::move(writes.effects.pre);
		for (auto& effect: writes.effects.post) body->body.push_back(std::move(effect));
		auto exists = awst::makeTupleItem(StorageMapper::makeBoxLenTuple(
			m_ctx.typeMapper, aggregate->key, m_loc), 1, awst::WType::boolType(), m_loc);
		m_ctx.queuePreEffect(awst::makeIfElse(std::move(exists), std::move(body), nullptr, m_loc));
		return;
	}
	if (auto const* slot = std::get_if<Slot>(&m_destination); slot && !m_type->isValueType())
	{
		EvmSlotLowering low(m_ctx, m_ctx.scope(), m_loc);
		if (!low.clearAggregate(slot->address, m_type, m_ctx.preEffects()))
			throw std::runtime_error("Cannot clear storage assignment destination");
		return;
	}
	if (auto destination = target())
	{
		auto target = awst::makeWritableTarget(std::move(destination));
		if (auto const* box = dynamic_cast<awst::BoxValueExpression const*>(target.get());
			box && !box->preserveEmptyBox)
		{
			m_ctx.queuePreExpression(awst::makeStateDelete(target, m_loc), m_loc);
			return;
		}
	}
	write(TypeCoercion::makeDefaultValue(m_native, m_loc));
}

} // namespace puyasol::builder::sol_ast
