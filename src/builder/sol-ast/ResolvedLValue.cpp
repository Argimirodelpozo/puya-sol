#include "builder/sol-ast/ResolvedLValue.h"
#include "builder/sol-ast/Context.h"
#include "builder/sol-ast/exprs/SolIndexAccess.h"
#include "builder/sol-eb/AssignmentHelper.h"
#include "builder/assembly/AssemblyBuilder.h"
#include "builder/contract/EvmMemoryCodec.h"
#include "builder/codec/EvmValueCodec.h"
#include "builder/storage/StorageMapper.h"
#include "builder/storage/TransientStorage.h"
#include "builder/sol-types/Arc4Defaults.h"
#include "builder/sol-types/TypeCoercion.h"
#include "builder/sol-types/TypeMapper.h"

#include <libsolidity/ast/AST.h>
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
	auto const* id = dynamic_cast<Identifier const*>(&source);
	auto const* decl = id ? dynamic_cast<VariableDeclaration const*>(id->annotation().referencedDeclaration) : nullptr;
	return decl && ctx.transientStorage && ctx.transientStorage->isTransient(*decl) ? decl : nullptr;
}

bool blobRoot(eb::ContractContext& ctx, Expression const& source)
{
	if (auto const* index = dynamic_cast<IndexAccess const*>(&source))
		return blobRoot(ctx, index->baseExpression());
	if (auto const* member = dynamic_cast<MemberAccess const*>(&source))
		return blobRoot(ctx, member->expression());
	auto const* id = dynamic_cast<Identifier const*>(&source);
	auto const* decl = id ? id->annotation().referencedDeclaration : nullptr;
	return decl && !ctx.scope().bindings.blobAggregates.get(decl->id()).empty();
}

Expr readable(Expr value, awst::SourceLocation const& loc)
{
	if (auto const* decode = dynamic_cast<awst::ARC4Decode const*>(value.get()))
		return awst::makeARC4Decode(readable(decode->value, loc), decode->wtype, loc);
	if (auto const* cast = dynamic_cast<awst::ReinterpretCast const*>(value.get()))
		return awst::makeReinterpretCast(readable(cast->expr, loc), cast->wtype, loc);
	if (dynamic_cast<awst::BoxValueExpression const*>(value.get()))
		return StorageMapper::makeStateGetWithDefault(value, value->wtype, loc);
	if (auto const* index = dynamic_cast<awst::IndexExpression const*>(value.get()))
		return awst::makeIndexExpression(readable(index->base, loc), index->index, index->wtype, loc);
	if (auto const* field = dynamic_cast<awst::FieldExpression const*>(value.get()))
		return awst::makeFieldExpression(readable(field->base, loc), field->name, field->wtype, loc);
	return value;
}
}

bool ResolvedLValue::isAddressed(eb::ContractContext& ctx, Expression const& source)
{
	return transientDeclaration(ctx, source)
		|| slotDestination(ctx, source) || blobRoot(ctx, source);
}

Expr ResolvedLValue::pin(Expr value)
{
	return m_ctx.emitSequencedOperand({}, std::move(value), true, m_loc);
}

ResolvedLValue::ResolvedLValue(eb::ContractContext& ctx, Expression const& source,
	awst::SourceLocation const& loc, Expr built)
	: m_ctx(ctx), m_type(source.annotation().type), m_native(ctx.typeMapper.map(m_type)), m_loc(loc)
{
	if ((m_transient = transientDeclaration(ctx, source))) return;
	if (!built && slotDestination(ctx, source))
	{
		EvmSlotLowering low(ctx, ctx.scope(), loc);
		auto const* index = dynamic_cast<IndexAccess const*>(&source);
		bool bytesElement = index && EvmSlotLowering::isBytesLike(index->baseExpression().annotation().type);
		m_slot = low.resolve(bytesElement ? index->baseExpression() : source);
		if (!m_slot) throw std::runtime_error("Cannot resolve storage assignment destination");
		m_slot->slot = pin(m_slot->slot);
		if (m_slot->byteOffset) m_slot->byteOffset = pin(m_slot->byteOffset);
		if (bytesElement)
		{
			m_byteIndex = pin(TypeCoercion::checkedIndexToUint64(ctx.preEffects(),
				ctx.pinIfWriteBacks(ctx.lower(*index->indexExpression(), false), loc), loc));
			ctx.queuePreExpression(awst::makeAssert(awst::makeNumericCompare(m_byteIndex,
				awst::NumericComparison::Lt, awst::makeLen(low.readBytesValue(*m_slot), loc), loc),
				loc, "bytes index out of range"), loc);
		}
		return;
	}
	if (!built && blobRoot(ctx, source))
	{
		m_blob = pin(SolIndexAccess::resolveBlobOffset(ctx, ctx.scope(), source, loc));
		if (!m_blob) throw std::runtime_error("Cannot resolve memory assignment destination");
		if (auto const* index = dynamic_cast<IndexAccess const*>(&source))
			m_packedBlobByte = EvmSlotLowering::isBytesLike(index->baseExpression().annotation().type);
		return;
	}
	if (!built)
	{
		auto lowered = ctx.lower(source, false);
		built = ctx.emitSequencedOperand(std::move(lowered.effects), std::move(lowered.value), false, loc);
	}
	m_target = resolveTarget(std::move(built));
	if (!m_target) throw std::runtime_error("Cannot resolve assignment destination");
}

Expr ResolvedLValue::resolveTarget(Expr target)
{
	// A later tuple store can change an index/key variable. Freeze addresses,
	// not container contents, while preserving the original assignable path.
	if (auto const* index = dynamic_cast<awst::IndexExpression const*>(target.get()))
	{
		auto base = resolveTarget(index->base);
		auto offset = pin(index->index);
		return awst::makeIndexExpression(std::move(base), std::move(offset), index->wtype, m_loc);
	}
	if (auto const* field = dynamic_cast<awst::FieldExpression const*>(target.get()))
		return awst::makeFieldExpression(resolveTarget(field->base), field->name, field->wtype, m_loc);
	if (auto const* decode = dynamic_cast<awst::ARC4Decode const*>(target.get()))
		return awst::makeARC4Decode(resolveTarget(decode->value), decode->wtype, m_loc);
	if (auto const* cast = dynamic_cast<awst::ReinterpretCast const*>(target.get()))
		return awst::makeReinterpretCast(resolveTarget(cast->expr), cast->wtype, m_loc);
	if (auto const* get = dynamic_cast<awst::StateGet const*>(target.get()))
	{
		auto resolved = std::make_shared<awst::StateGet>(*get);
		resolved->field = resolveTarget(get->field);
		return resolved;
	}
	if (auto const* box = dynamic_cast<awst::BoxValueExpression const*>(target.get()))
	{
		auto resolved = std::make_shared<awst::BoxValueExpression>(*box);
		resolved->key = pin(box->key);
		return resolved;
	}
	return target;
}

Expr ResolvedLValue::read()
{
	if (m_transient) return m_ctx.transientStorage->buildRead(*m_transient, m_loc);
	if (m_slot)
	{
		EvmSlotLowering low(m_ctx, m_ctx.scope(), m_loc);
		if (m_byteIndex) return awst::makeExtract3(low.readBytesValue(*m_slot), m_byteIndex,
			awst::makeOne(m_loc), m_loc, m_native);
		return low.readAny(*m_slot, m_type);
	}
	if (m_blob) return SolIndexAccess::readBlobValue(m_ctx, m_blob, m_type, m_loc);
	auto value = StorageMapper::makePartialBoxReadWithDefault(
		m_ctx.typeMapper, readable(m_target, m_loc), m_ctx.preEffects(), m_loc);
	if (auto const* index = dynamic_cast<awst::IndexExpression const*>(value.get());
		index && index->base->wtype->kind() == awst::WTypeKind::Bytes)
		value = awst::makeExtract3(index->base, index->index, awst::makeOne(m_loc), m_loc, m_native);
	if (isArc4EncodedType(value->wtype) && !isArc4EncodedType(m_native))
		value = awst::makeARC4Decode(std::move(value), m_native, m_loc);
	return TypeCoercion::signExtendSignedElement(std::move(value), m_type, m_loc);
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
	value = pin(TypeCoercion::coerceForAssignment(std::move(value), m_native, m_loc, &m_ctx.preEffects()));
	if (m_transient)
		m_ctx.queuePreEffect(m_ctx.transientStorage->buildWrite(*m_transient, value, m_loc));
	else if (m_slot)
	{
		EvmSlotLowering low(m_ctx, m_ctx.scope(), m_loc);
		if (m_byteIndex)
			low.writeBytesValue(*m_slot, awst::makeReplace3(low.readBytesValue(*m_slot),
				m_byteIndex, awst::makeAsBytes(value, m_loc), m_loc), m_ctx.preEffects());
		else if (!low.writeAny(*m_slot, m_type, value, m_ctx.preEffects()))
			throw std::runtime_error("Cannot write storage assignment destination");
		if (!m_type->isValueType()) return m_slot->slot;
	}
	else if (m_blob)
	{
		if (m_packedBlobByte)
			AssemblyBuilder::writeMemByteDirect(m_ctx.typeMapper.profile().scratchLayout, m_blob,
				awst::makeExtract(codec::valueToEvmWord(m_ctx.typeMapper, m_type, value, m_loc), 0, 1, m_loc),
				m_loc, m_ctx.preEffects());
		else if (!writeEvmMemoryValueAt(m_ctx.typeMapper, m_type, value, m_blob, m_loc, m_ctx.preEffects()))
			throw std::runtime_error("Cannot write memory assignment destination");
	}
	else
	{
		writeTarget(m_target, value);
		if (!m_type->isValueType() && m_type->dataStoredIn(DataLocation::Storage))
			return readable(m_target, m_loc);
	}
	return value;
}

void ResolvedLValue::clear()
{
	if (m_slot && !m_type->isValueType())
	{
		EvmSlotLowering low(m_ctx, m_ctx.scope(), m_loc);
		if (!low.clearAggregate(*m_slot, m_type, m_ctx.preEffects()))
			throw std::runtime_error("Cannot clear storage assignment destination");
		return;
	}
	if (m_target)
	{
		auto target = awst::makeWritableTarget(m_target);
		if (auto const* box = dynamic_cast<awst::BoxValueExpression const*>(target.get());
			box && !StorageMapper::isTopLevelDynamicBox(box))
		{
			m_ctx.queuePreExpression(awst::makeStateDelete(target, m_loc), m_loc);
			return;
		}
	}
	write(StorageMapper::makeDefaultValue(m_native, m_loc));
}

} // namespace puyasol::builder::sol_ast
