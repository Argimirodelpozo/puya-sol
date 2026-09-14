/// @file SolIndexAccess.cpp

#include "builder/ast/exprs/SolIndexAccess.h"
#include "builder/storage/slot/EvmSlotLowering.h"
#include "awst/NameGen.h"
#include "builder/eb/NodeBuilder.h"
#include "builder/target/EvmLayoutMode.h"
#include "builder/storage/StorageMapper.h"
#include "builder/storage/slot/SlotHandleAccess.h"
#include "builder/types/TypeMapper.h"
#include "builder/codec/Arc4Defaults.h"
#include "builder/types/TypeCoercion.h"
#include "builder/yul/AssemblyBuilder.h"
#include "builder/codec/EvmMemoryCodec.h"
#include "awst/WType.h"
#include "Logger.h"

#include <libsolidity/ast/AST.h>
#include <libsolidity/ast/TypeProvider.h>

namespace puyasol::builder::sol_ast
{

using namespace solidity::frontend;

SolIndexAccess::SolIndexAccess(eb::ContractContext& _ctx, IndexAccess const& _node)
	: SolExpression(_ctx, _node), m_indexAccess(_node)
{
}

std::shared_ptr<awst::Expression> SolIndexAccess::toAwst()
{
	// `S[7][]` in expression position is an array type, not an element read.
	if (dynamic_cast<TypeType const*>(m_indexAccess.annotation().type))
		return awst::makeVoidConstant(m_loc);
	auto const* baseType = m_indexAccess.baseExpression().annotation().type;

	// --evm-storage-layout: reads rooted at a persistent state var resolve to
	// their EVM word address (writes intercept in SolAssignment and never
	// build the index expression).
	if (m_ctx.typeMapper.profile().evmStorageLayout
		&& EvmSlotLowering::isStorageStateRef(m_indexAccess))
	{
		EvmSlotLowering low(m_ctx, m_scope, m_loc);
		// bytes/string element read: whole-value read + byte extract (the
		// short/long form logic lives in __evm_bytes_read). Bounds-assert
		// mirrors EVM Panic 0x32 — an OOB byte read must not fall off the end.
		if (auto const* bbat = dynamic_cast<ArrayType const*>(baseType);
			bbat && bbat->isByteArrayOrString()
			&& bbat->dataStoredIn(DataLocation::Storage)
			&& m_indexAccess.indexExpression())
		{
			auto baseAddr = low.resolve(m_indexAccess.baseExpression());
			if (!baseAddr)
				return nullptr;
			baseAddr->solType = baseType;
			auto whole = low.readBytesValue(*baseAddr);
			if (whole && whole->wtype != awst::WType::bytesType())
				whole = awst::makeAsBytes(std::move(whole), m_loc);
			std::string nm = "__evm_bi_" + std::to_string(
				awst::NameGen::next("SolIndexAccess.bytesElem"));
			m_ctx.queuePreEffect(awst::makeAssignmentStatement(
				awst::makeVarExpression(nm, awst::WType::bytesType(), m_loc),
				std::move(whole), m_loc));
			auto wv = [&]() {
				return awst::makeVarExpression(
					nm, awst::WType::bytesType(), m_loc);
			};
			auto idx = buildExpr(*m_indexAccess.indexExpression());
			if (!idx)
				return nullptr;
			{
				std::vector<std::shared_ptr<awst::Statement>> idxPre;
				idx = TypeCoercion::checkedIndexToUint64(idxPre, std::move(idx), m_loc);
				for (auto& ps: idxPre)
					m_ctx.queuePreEffect(std::move(ps));
			}
			idx = awst::makeEvalOnce(std::move(idx), m_loc);
			auto inBounds = awst::makeNumericCompare(idx,
				awst::NumericComparison::Lt, awst::makeLen(wv(), m_loc), m_loc);
			m_ctx.queuePreEffect(awst::makeExpressionStatement(
				awst::makeAssert(std::move(inBounds), m_loc,
					"bytes index out of range"), m_loc));
			auto one = awst::makeExtract3(wv(), idx,
				awst::makeIntegerConstant(uint64_t{1}, m_loc), m_loc);
			auto const* resW =
				m_ctx.typeMapper.map(m_indexAccess.annotation().type);
			if (resW && resW != awst::WType::bytesType())
				return awst::makeReinterpretCast(std::move(one), resW, m_loc);
			return one;
		}
		auto addr = low.resolve(m_indexAccess);
		if (!addr)
			return nullptr;
		auto const* resType = m_indexAccess.annotation().type;
		return low.readAny(*addr, resType);
	}

	// Explicit `.slot` storage handles use the same recursive Solidity layout
	// resolver as EVM-layout state. One path covers a[i], a[i][j], nested
	// structs, and any further rank; assignment intercepts consume the resolved
	// address before an lvalue is built.
	auto const* slotResultType = m_indexAccess.annotation().type;
	if (EvmSlotLowering::isSlotHandleRef(m_indexAccess, m_ctx, m_scope))
	{
		EvmSlotLowering low(m_ctx, m_scope, m_loc);
		auto addr = low.resolve(m_indexAccess);
		if (!addr)
			return nullptr;
		if (m_indexAccess.annotation().willBeWrittenTo)
			return addr->slot;
		return low.readAny(*addr, slotResultType);
	}

	if (auto result = handleSlicedIndex()) return result;

	// Box-backed array access. State variables need this direct route for
	// dynamic roots; storage-ref params need it for every recursively dynamic
	// root because their runtime value is a box key, not an array value.
	bool isDynamicArrayAccess = false;
	if (auto const* arrType = dynamic_cast<ArrayType const*>(baseType))
	{
		if (auto const* ident = dynamic_cast<Identifier const*>(
				&m_indexAccess.baseExpression()))
		{
			if (auto const* varDecl = dynamic_cast<VariableDeclaration const*>(
					ident->annotation().referencedDeclaration))
			{
				if ((varDecl->isStateVariable() && arrType->isDynamicallySized()
						&& !varDecl->isConstant() && !varDecl->immutable())
					|| (!m_scope.bindings.mappingKeyParams.get(varDecl->id()).empty()
						&& !arrType->isByteArrayOrString()))
					isDynamicArrayAccess = true;
			}
		}
	}

	if (isDynamicArrayAccess)
		return handleDynamicArrayAccess();

	if (dynamic_cast<MappingType const*>(baseType))
		return handleMappingAccess();

	// Blob-backed memory aggregate scalar-leaf READ: `a[i]`, `p.field[i][j]`,
	// `p.f.x` where the chain roots at a >4KB memory aggregate (registered in
	// SolVariableDeclaration). Writes are handled in SolAssignment; aggregates
	// <=4KB are not registered and fall through to the value model below.
	if (!m_indexAccess.annotation().willBeWrittenTo)
	{
		if (auto off = resolveBlobOffset(m_ctx, m_scope, m_indexAccess, m_loc))
			if (auto val = readBlobValue(
					m_ctx, std::move(off), m_indexAccess.annotation().type, m_loc))
				return val;
	}

	return handleRegularIndex();
}

std::optional<eb::ContractContext::LoweredExpression> SolIndexAccess::resolveBlobReference(
	eb::ContractContext& ctx, Context& scope, solidity::frontend::Expression const& node,
	awst::SourceLocation const& loc)
{
	auto result = ctx.lowerOperand([&] { return resolveBlobOffset(ctx, scope, node, loc); }, false);
	if (!result.value) return std::nullopt;
	return eb::ContractContext::LoweredExpression{std::move(result.value), std::move(result.effects), node.annotation().type};
}

std::shared_ptr<awst::Expression> SolIndexAccess::resolveBlobOffset(
	eb::ContractContext& _ctx, Context& _scope,
	solidity::frontend::Expression const& _node, awst::SourceLocation const& _loc)
{
	using namespace solidity::frontend;

	if (auto const* tuple = dynamic_cast<TupleExpression const*>(&_node);
		tuple && tuple->components().size() == 1 && tuple->components()[0])
		return resolveBlobOffset(_ctx, _scope, *tuple->components()[0], _loc);
	if (auto const* call = dynamic_cast<FunctionCall const*>(&_node);
		call && call->annotation().kind.set() && *call->annotation().kind == FunctionCallKind::TypeConversion
		&& call->arguments().size() == 1 && !_node.annotation().type->isValueType())
		return resolveBlobOffset(_ctx, _scope, *call->arguments()[0], _loc);
	if (auto const* conditional = dynamic_cast<Conditional const*>(&_node))
	{
		auto branch = [&](auto const& expression) {
			return _ctx.lowerOperand([&] { return resolveBlobOffset(_ctx, _scope, expression, _loc); });
		};
		auto yes = branch(conditional->trueExpression()), no = branch(conditional->falseExpression());
		// A fresh branch must not force an existing reference into the value
		// model: that would copy its object and lose alias identity. Allocate
		// only the fresh branch, inside its conditional effect region.
		auto const* referenceType = dynamic_cast<ReferenceType const*>(_node.annotation().type);
		if ((yes.value || no.value) && referenceType
			&& referenceType->location() == DataLocation::Memory)
		{
			auto spill = [&](auto const& expression) {
				return _ctx.lowerOperand([&]() -> std::shared_ptr<awst::Expression> {
					auto value = _ctx.pinIfWriteBacks(_ctx.lower(expression, false), _loc);
					if (!value) return nullptr;
					auto const* wtype = _ctx.typeMapper.map(referenceType);
					value = TypeCoercion::coerceForAssignment(std::move(value), wtype, _loc);
					auto id = awst::NameGen::next("SolIndexAccess.freshReference");
					auto name = "__ref_fresh_" + std::to_string(id);
					if (!spillEvmMemoryValue(_ctx.typeMapper, referenceType, wtype,
						std::move(value), name, id, _loc, _ctx.preEffects()))
						return nullptr;
					return awst::makeVarExpression(name, awst::WType::uint64Type(), _loc);
				});
			};
			if (!yes.value) yes = spill(conditional->trueExpression());
			if (!no.value) no = spill(conditional->falseExpression());
		}
		if (!yes.value || !no.value) return nullptr;
		auto condition = _ctx.pinIfWriteBacks(_ctx.lower(conditional->condition(), false), _loc);
		auto pointer = awst::makeVarExpression("__ref_select_" + std::to_string(
			awst::NameGen::next("SolIndexAccess.pointerSelect")), awst::WType::uint64Type(), _loc);
		auto yesBlock = eb::ContractContext::makeScopedResultBlock(std::move(yes.effects.pre),
			pointer, std::move(yes.value), _loc, std::move(yes.effects.post));
		auto noBlock = eb::ContractContext::makeScopedResultBlock(std::move(no.effects.pre),
			pointer, std::move(no.value), _loc, std::move(no.effects.post));
		_ctx.preEffects().push_back(awst::makeIfElse(std::move(condition), std::move(yesBlock), std::move(noBlock), _loc));
		return pointer;
	}

	// Root: Identifier referencing a blob-backed aggregate local → its base offset.
	if (auto const* ident = dynamic_cast<Identifier const*>(&_node))
	{
		auto const* vd = dynamic_cast<VariableDeclaration const*>(
			ident->annotation().referencedDeclaration);
		if (!vd) return nullptr;
		std::string offVar = _scope.bindings.blobAggregates.get(vd->id());
		if (offVar.empty()) return nullptr;
		return awst::makeVarExpression(offVar, awst::WType::uint64Type(), _loc);
	}

	// `base[i]` → the element's EVM-memory address.  solc owns the stride;
	// reference elements occupy pointer slots which are followed recursively.
	if (auto const* ia = dynamic_cast<IndexAccess const*>(&_node))
	{
		if (!ia->indexExpression()) return nullptr;
		auto const* baseArr = dynamic_cast<ArrayType const*>(ia->baseExpression().annotation().type);
		if (!baseArr) return nullptr;
		auto parent = resolveBlobOffset(_ctx, _scope, ia->baseExpression(), _loc);
		if (!parent) return nullptr;
		auto idx = _ctx.pinIfWriteBacks(_ctx.lower(*ia->indexExpression(), false), _loc);
		idx = builder::TypeCoercion::checkedIndexToUint64(
			_ctx.preEffects(), std::move(idx), _loc);
		std::shared_ptr<awst::Expression> base = std::move(parent);
		std::shared_ptr<awst::Expression> count;
		if (baseArr->isDynamicallySized())
		{
			count = builder::readEvmMemoryUint64Word(
				_ctx.typeMapper, base, _loc, _ctx.preEffects());
			base = awst::makeUInt64BinOp(std::move(base),
				awst::UInt64BinaryOperator::Add,
				awst::makeIntegerConstant(uint64_t{32}, _loc), _loc);
		}
		else
			count = awst::makeIntegerConstant(
				static_cast<uint64_t>(baseArr->length()), _loc);
		idx = awst::makeEvalOnce(std::move(idx), _loc);
		_ctx.queuePreEffect(awst::makeExpressionStatement(
			awst::makeAssert(awst::makeNumericCompare(
				idx, awst::NumericComparison::Lt, std::move(count), _loc),
				_loc, "memory array index out of range"), _loc));
		uint64_t stride = baseArr->isByteArrayOrString()
			? uint64_t{1} : static_cast<uint64_t>(baseArr->memoryStride());
		auto slot = awst::makeUInt64BinOp(std::move(base), awst::UInt64BinaryOperator::Add,
			awst::makeUInt64BinOp(std::move(idx), awst::UInt64BinaryOperator::Mult,
				awst::makeIntegerConstant(stride, _loc), _loc), _loc);
		auto const* resultType = ia->annotation().type;
		if (!baseArr->isByteArrayOrString()
			&& (dynamic_cast<ArrayType const*>(resultType)
				|| dynamic_cast<StructType const*>(resultType)))
			return builder::readEvmMemoryUint64Word(
				_ctx.typeMapper, std::move(slot), _loc, _ctx.preEffects());
		return slot;
	}

	// `base.field` → parentOffset + sum of encoded sizes of preceding members.
	if (auto const* ma = dynamic_cast<MemberAccess const*>(&_node))
	{
		auto const* structType = dynamic_cast<StructType const*>(
			ma->expression().annotation().type);
		if (!structType) return nullptr;
		auto parent = resolveBlobOffset(_ctx, _scope, ma->expression(), _loc);
		if (!parent) return nullptr;
		uint64_t fieldOff = static_cast<uint64_t>(
			structType->memoryOffsetOfMember(ma->memberName()));
		auto slot = fieldOff == 0 ? std::move(parent)
			: awst::makeUInt64BinOp(std::move(parent), awst::UInt64BinaryOperator::Add,
				awst::makeIntegerConstant(fieldOff, _loc), _loc);
		auto const* resultType = ma->annotation().type;
		if (dynamic_cast<ArrayType const*>(resultType)
			|| dynamic_cast<StructType const*>(resultType))
			return builder::readEvmMemoryUint64Word(
				_ctx.typeMapper, std::move(slot), _loc, _ctx.preEffects());
		return slot;
	}

	return nullptr;
}

std::shared_ptr<awst::Expression> SolIndexAccess::readBlobValue(
	eb::ContractContext& _ctx, std::shared_ptr<awst::Expression> _off,
	solidity::frontend::Type const* _solType, awst::SourceLocation const& _loc)
{
	using namespace solidity::frontend;

	auto* mapped = _ctx.typeMapper.map(_solType);
	return builder::materializeEvmMemoryValue(
		_ctx.typeMapper, _solType, mapped, std::move(_off), _loc,
		_ctx.preEffects());
}

// ── IndexRangeAccess ──

SolIndexRangeAccess::SolIndexRangeAccess(
	eb::ContractContext& _ctx, IndexRangeAccess const& _node)
	: SolExpression(_ctx, _node), m_rangeAccess(_node)
{
}

std::optional<SolIndexRangeAccess::Slice> SolIndexRangeAccess::resolveSlice(
	eb::ContractContext& ctx, Expression const& source, awst::SourceLocation const& loc)
{
	auto peel = [](Expression const& expression) {
		auto const* current = &expression;
		for (;;)
		{
			if (auto const* tuple = dynamic_cast<TupleExpression const*>(current);
				tuple && tuple->components().size() == 1 && tuple->components()[0])
				current = tuple->components()[0].get();
			else if (auto const* call = dynamic_cast<FunctionCall const*>(current);
				call && *call->annotation().kind == FunctionCallKind::TypeConversion)
				current = call->arguments()[0].get();
			else return current;
		}
	};
	std::vector<IndexRangeAccess const*> ranges;
	auto const* root = peel(source);
	while (auto const* range = dynamic_cast<IndexRangeAccess const*>(root))
	{
		ranges.push_back(range);
		root = peel(range->baseExpression());
	}
	auto const* type = dynamic_cast<ArrayType const*>(root->annotation().type);
	if (ranges.empty() || !type || type->isByteArrayOrString()) return std::nullopt;

	auto lowered = ctx.lower(*root, false);
	auto base = ctx.emitSequencedOperand(
		std::move(lowered.effects), std::move(lowered.value), true, loc);
	std::shared_ptr<awst::Expression> offset = awst::makeZero(loc);
	auto length = ctx.emitSequencedOperand({},
		awst::makeArrayLength(base, awst::WType::uint64Type(), loc), true, loc);
	for (auto range = ranges.rbegin(); range != ranges.rend(); ++range)
	{
		auto [start, end] = resolveBounds(ctx, **range, length, loc);
		offset = ctx.emitSequencedOperand({}, awst::makeUInt64BinOp(
			offset, awst::UInt64BinaryOperator::Add, start, loc), true, loc);
		length = ctx.emitSequencedOperand({}, awst::makeUInt64BinOp(
			end, awst::UInt64BinaryOperator::Sub, start, loc), true, loc);
	}
	return Slice{std::move(base), std::move(offset), std::move(length)};
}

SolIndexRangeAccess::Bounds SolIndexRangeAccess::resolveBounds(
	eb::ContractContext& ctx, IndexRangeAccess const& range,
	std::shared_ptr<awst::Expression> length, awst::SourceLocation const& loc)
{
	auto bound = [&](Expression const* expression, std::shared_ptr<awst::Expression> fallback) {
		auto value = expression ? ctx.pinIfWriteBacks(ctx.lower(*expression, false), loc) : fallback;
		value = TypeCoercion::checkedIndexToUint64(ctx.preEffects(), std::move(value), loc);
		return ctx.emitSequencedOperand({}, std::move(value), true, loc);
	};
	auto start = bound(range.startExpression(), awst::makeZero(loc));
	auto end = bound(range.endExpression(), length);
	ctx.queuePreExpression(awst::makeAssert(awst::makeNumericCompare(
		start, awst::NumericComparison::Lte, end, loc), loc, "slice: start > end"), loc);
	ctx.queuePreExpression(awst::makeAssert(awst::makeNumericCompare(
		end, awst::NumericComparison::Lte, length, loc), loc, "slice: end > length"), loc);
	return {std::move(start), std::move(end)};
}

std::shared_ptr<awst::Expression> SolIndexRangeAccess::toAwst()
{
	auto base = m_ctx.emitSequencedOperand({},
		m_ctx.pinIfWriteBacks(m_ctx.lower(m_rangeAccess.baseExpression(), false), m_loc), true, m_loc);
	auto const* element = awst::arrayElementType(base->wtype);
	auto length = element
		? awst::makeArrayLength(base, awst::WType::uint64Type(), m_loc)
		: std::shared_ptr<awst::Expression>(awst::makeLen(base, m_loc));
	auto [start, end] = resolveBounds(m_ctx, m_rangeAccess, length, m_loc);
	auto const* resultType = m_ctx.typeMapper.map(m_rangeAccess.annotation().type);
	if (element)
	{
		auto stride = computeEncodedElementSize(element).fixedBytes<int>();
		if (stride && element != awst::WType::arc4BoolType())
		{
			int header = base->wtype->kind() == awst::WTypeKind::ARC4DynamicArray ? 2 : 0;
			auto offset = [&](auto index) {
				return awst::makeUInt64BinOp(awst::makeUInt64BinOp(index,
					awst::UInt64BinaryOperator::Mult, awst::makeIntegerConstant(*stride, m_loc), m_loc),
					awst::UInt64BinaryOperator::Add, awst::makeIntegerConstant(header, m_loc), m_loc);
			};
			auto bytes = awst::makeIntrinsicCall("substring3", awst::WType::bytesType(), m_loc);
			bytes->stackArgs = {awst::makeAsBytes(base, m_loc), offset(start), offset(end)};
			auto count = awst::makeUInt64BinOp(end, awst::UInt64BinaryOperator::Sub, start, m_loc);
			return awst::makeReinterpretCast(awst::makeConcat(
				awst::makeUInt16Bytes(std::move(count), m_loc), bytes, m_loc), resultType, m_loc);
		}
		// ARC4 bool arrays are bit-packed; variable-size elements have offsets.
		// Let the typed array operations handle those encodings instead of slicing bytes.
		auto result = m_ctx.emitSequencedOperand({}, awst::makeNewArray(resultType, m_loc), true, m_loc);
		auto index = awst::makeVarExpression("__slice_copy_" + std::to_string(m_rangeAccess.id()),
			awst::WType::uint64Type(), m_loc);
		m_ctx.queuePreEffect(awst::makeAssignmentStatement(index, start, m_loc));
		auto body = awst::makeBlock(m_loc);
		body->body.push_back(awst::makeExpressionStatement(awst::makeArrayPushOne(
			result, awst::makeIndexExpression(base, index, element, m_loc), resultType, m_loc), m_loc));
		body->body.push_back(awst::makeAssignmentStatement(index, awst::makeUInt64BinOp(index,
			awst::UInt64BinaryOperator::Add, awst::makeOne(m_loc), m_loc), m_loc));
		m_ctx.queuePreEffect(awst::makeWhileLoop(awst::makeNumericCompare(
			index, awst::NumericComparison::Lt, end, m_loc), body, m_loc));
		return result;
	}
	auto slice = awst::makeIntrinsicCall("substring3", resultType, m_loc);
	slice->stackArgs = {base, start, end};
	return slice;
}

} // namespace puyasol::builder::sol_ast
