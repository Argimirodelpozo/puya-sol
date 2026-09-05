/// @file SolIndexAccess.cpp

#include "builder/sol-ast/exprs/SolIndexAccess.h"
#include "builder/sol-ast/EvmSlotLowering.h"
#include "awst/NameGen.h"
#include "builder/sol-eb/NodeBuilder.h"
#include "builder/storage/EvmLayoutMode.h"
#include "builder/storage/StorageMapper.h"
#include "builder/storage/SlotHandleAccess.h"
#include "builder/sol-types/TypeMapper.h"
#include "builder/sol-types/Arc4Defaults.h"
#include "builder/sol-types/TypeCoercion.h"
#include "builder/assembly/AssemblyBuilder.h"
#include "builder/contract/EvmMemoryCodec.h"
#include "awst/WType.h"
#include "Logger.h"

#include <libsolidity/ast/AST.h>
#include <libsolidity/ast/TypeProvider.h>

namespace puyasol::builder::sol_ast
{

using namespace solidity::frontend;

namespace
{
// Read a FULL-WIDTH biguint storage slot via __puyasol___storage_read (the
// box-per-slot store keys on all 32 bytes; the historical low-8 truncation was
// only sound under the mod-256 fallback).
std::shared_ptr<awst::Expression> readStorageSlotBiguint(
	std::shared_ptr<awst::Expression> _slot, awst::SourceLocation const& _loc)
{
	auto call = awst::makeSubroutineCall(
		awst::SubroutineID{"__puyasol___storage_read"}, awst::WType::biguintType(), _loc);
	awst::pushCallArg(call->args, "__slot", std::move(_slot));
	return call;
}
} // namespace

SolIndexAccess::SolIndexAccess(eb::ContractContext& _ctx, IndexAccess const& _node)
	: SolExpression(_ctx, _node), m_indexAccess(_node)
{
}

std::shared_ptr<awst::Expression> SolIndexAccess::toAwst()
{
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

	// Slice indexing `root[a:b]...[i]`: fold the slice chain into a direct index;
	// bytes-substring3 would produce bytes[1] instead of the declared element type.
	{
		auto const* peeled = &m_indexAccess.baseExpression();
		while (auto const* call = dynamic_cast<solidity::frontend::FunctionCall const*>(peeled))
		{
			if (call->annotation().kind.set()
				&& *call->annotation().kind == solidity::frontend::FunctionCallKind::TypeConversion
				&& !call->arguments().empty())
				peeled = call->arguments()[0].get();
			else
				break;
		}
		if (dynamic_cast<solidity::frontend::IndexRangeAccess const*>(peeled))
		{
			if (auto result = handleSlicedIndex())
				return result;
		}
	}

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
					|| (!m_scope.findMappingKeyParam(varDecl->id()).empty()
						&& !arrType->isByteArrayOrString()))
					isDynamicArrayAccess = true;
			}
		}
	}

	if (isDynamicArrayAccess)
		return handleDynamicArrayAccess();

	// Nested mapping check
	bool isNestedMappingAccess = false;
	if (auto const* baseIndexAccess = dynamic_cast<IndexAccess const*>(
			&m_indexAccess.baseExpression()))
	{
		auto const* innerBaseType = baseIndexAccess->baseExpression().annotation().type;
		if (innerBaseType && innerBaseType->category() == Type::Category::Mapping)
		{
			auto const* innerMapping = dynamic_cast<MappingType const*>(innerBaseType);
			if (innerMapping && innerMapping->valueType()->category() == Type::Category::Mapping)
				isNestedMappingAccess = true;
		}
	}

	if (baseType && (baseType->category() == Type::Category::Mapping || isNestedMappingAccess))
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

std::shared_ptr<awst::Expression> SolIndexAccess::resolveBlobOffset(
	eb::ContractContext& _ctx, Context& _scope,
	solidity::frontend::Expression const& _node, awst::SourceLocation const& _loc)
{
	using namespace solidity::frontend;

	// Root: Identifier referencing a blob-backed aggregate local → its base offset.
	if (auto const* ident = dynamic_cast<Identifier const*>(&_node))
	{
		auto const* vd = dynamic_cast<VariableDeclaration const*>(
			ident->annotation().referencedDeclaration);
		if (!vd) return nullptr;
		std::string offVar = _scope.findBlobAggregate(vd->id());
		if (offVar.empty()) return nullptr;
		return awst::makeVarExpression(offVar, awst::WType::uint64Type(), _loc);
	}

	// `base[i]` → the element's EVM-memory address.  solc owns the stride;
	// reference elements occupy pointer slots which are followed recursively.
	if (auto const* ia = dynamic_cast<IndexAccess const*>(&_node))
	{
		if (!ia->indexExpression()) return nullptr;
		auto parent = resolveBlobOffset(_ctx, _scope, ia->baseExpression(), _loc);
		if (!parent) return nullptr;
		auto idx = _ctx.buildExpr(*ia->indexExpression());
		idx = builder::TypeCoercion::implicitNumericCast(
			std::move(idx), awst::WType::uint64Type(), _loc);
		auto const* baseArr = dynamic_cast<ArrayType const*>(
			ia->baseExpression().annotation().type);
		if (!baseArr) return nullptr;
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
		auto parent = resolveBlobOffset(_ctx, _scope, ma->expression(), _loc);
		if (!parent) return nullptr;
		auto const* structType = dynamic_cast<StructType const*>(
			ma->expression().annotation().type);
		if (!structType) return nullptr;
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

std::shared_ptr<awst::Expression> SolIndexRangeAccess::toAwst()
{
	// T2: the slice lowering references the base several times (length assert,
	// start/end scaling, the substring itself) — pin so a call-valued base
	// evaluates once. Pure leaves pass through.
	auto base = awst::makeEvalOnce(buildExpr(m_rangeAccess.baseExpression()), m_loc);

	std::shared_ptr<awst::Expression> start;
	if (m_rangeAccess.startExpression())
		start = buildExpr(*m_rangeAccess.startExpression());
	else
	{
		auto zero = awst::makeZero(m_loc);
		start = std::move(zero);
	}

	std::shared_ptr<awst::Expression> end;
	if (m_rangeAccess.endExpression())
		end = buildExpr(*m_rangeAccess.endExpression());
	else
	{
		// Default end for substring3: byte-count via `len` intrinsic,
		// preserving pre-existing full-slice semantics.
		end = awst::makeLen(base, m_loc);
	}

	start = builder::TypeCoercion::implicitNumericCast(
		std::move(start), awst::WType::uint64Type(), m_loc);
	end = builder::TypeCoercion::implicitNumericCast(
		std::move(end), awst::WType::uint64Type(), m_loc);

	// Bounds checks for explicit `arr[start:end]` — Solidity reverts on
	// start > end or end > arr.length even if the slice result is unused.
	// Stash bounds in temps and emit asserts via pre-effects so
	// they survive DCE when the slice expression is discarded. Only applied
	// when the user supplied at least one explicit bound; default `[:]`
	// slices are by construction in-range and keep the old semantics.
	bool hasExplicitBound
		= m_rangeAccess.startExpression() || m_rangeAccess.endExpression();

	if (hasExplicitBound)
	{
		std::string idSuffix = std::to_string(m_rangeAccess.id());
		std::string startVarName = "__slice_start_" + idSuffix;
		std::string endVarName = "__slice_end_" + idSuffix;

		auto startVar = awst::makeVarExpression(startVarName, awst::WType::uint64Type(), m_loc);
		m_ctx.preEffects().push_back(
			awst::makeAssignmentStatement(startVar, start, m_loc));

		auto endVar = awst::makeVarExpression(endVarName, awst::WType::uint64Type(), m_loc);
		m_ctx.preEffects().push_back(
			awst::makeAssignmentStatement(endVar, end, m_loc));

		// Every path below, including the generic substring fallback, must use
		// the bound values captured above.  Keeping the original expressions
		// here made side-effecting bounds execute once for the checks and again
		// for the actual slice.
		start = awst::makeVarExpression(
			startVarName, awst::WType::uint64Type(), m_loc);
		end = awst::makeVarExpression(
			endVarName, awst::WType::uint64Type(), m_loc);

		// assert(start <= end)
		{
			auto cmp = awst::makeNumericCompare(
				awst::makeVarExpression(startVarName, awst::WType::uint64Type(), m_loc),
				awst::NumericComparison::Lte,
				awst::makeVarExpression(endVarName, awst::WType::uint64Type(), m_loc),
				m_loc);
			m_ctx.preEffects().push_back(awst::makeExpressionStatement(
				awst::makeAssert(std::move(cmp), m_loc, "slice: start > end"), m_loc));
		}

		// assert(end <= base.length) — only for base shapes that support a
		// length query. Inner slices that fell back to bytes-of-unknown-shape
		// skip this check.
		auto const* bt = base->wtype;
		std::shared_ptr<awst::Expression> lenExpr;
		if (dynamic_cast<awst::ReferenceArray const*>(bt)
			|| dynamic_cast<awst::ARC4DynamicArray const*>(bt)
			|| dynamic_cast<awst::ARC4StaticArray const*>(bt))
		{
			lenExpr = awst::makeArrayLength(base, awst::WType::uint64Type(), m_loc);
		}

		if (lenExpr)
		{
			auto cmp = awst::makeNumericCompare(
				awst::makeVarExpression(endVarName, awst::WType::uint64Type(), m_loc),
				awst::NumericComparison::Lte,
				std::move(lenExpr),
				m_loc);
			m_ctx.preEffects().push_back(awst::makeExpressionStatement(
				awst::makeAssert(std::move(cmp), m_loc, "slice: end > length"), m_loc));
		}
	}

	auto const* resultType = m_ctx.typeMapper.map(m_rangeAccess.annotation().type);

	// For ARC4-encoded array bases (ARC4DynamicArray / ARC4StaticArray) with
	// fixed-size elements, `start`/`end` are ELEMENT indices — a raw
	// substring3 would yield malformed bytes. Emit an arc4-aware slice:
	// concat(uint16 BE (end - start), substring3(base, hdr + s*elem, hdr + e*elem)).
	// Bytes/string slices fall through to substring3 below.
	if (hasExplicitBound)
	{
		awst::WType const* elemType = nullptr;
		int64_t headerBytes = 0;
		auto const* bt = base->wtype;
		if (auto const* ad = dynamic_cast<awst::ARC4DynamicArray const*>(bt))
		{
			elemType = ad->elementType();
			headerBytes = 2;
		}
		else if (auto const* as = dynamic_cast<awst::ARC4StaticArray const*>(bt))
		{
			elemType = as->elementType();
			headerBytes = 0;
		}

		int elemSize = elemType ? builder::computeEncodedElementSize(elemType).fixedBytes<int>().value_or(0) : 0;

		if (elemSize > 0)
		{
			std::string idSuffix = std::to_string(m_rangeAccess.id());
			std::string startVarName = "__slice_start_" + idSuffix;
			std::string endVarName = "__slice_end_" + idSuffix;

			auto mkStart = [&]() {
				return awst::makeVarExpression(startVarName, awst::WType::uint64Type(), m_loc);
			};
			auto mkEnd = [&]() {
				return awst::makeVarExpression(endVarName, awst::WType::uint64Type(), m_loc);
			};

			auto scaled = [&](std::shared_ptr<awst::Expression> idx) {
				auto scale = awst::makeUInt64BinOp(
					std::move(idx),
					awst::UInt64BinaryOperator::Mult,
					awst::makeIntegerConstant(elemSize, m_loc),
					m_loc);
				if (headerBytes > 0)
				{
					return awst::makeUInt64BinOp(
						std::move(scale),
						awst::UInt64BinaryOperator::Add,
						awst::makeIntegerConstant(headerBytes, m_loc),
						m_loc);
				}
				return scale;
			};

			auto byteStart = scaled(mkStart());
			auto byteEnd = scaled(mkEnd());

			auto sub = awst::makeIntrinsicCall("substring3", awst::WType::bytesType(), m_loc);
			sub->stackArgs.push_back(std::move(base));
			sub->stackArgs.push_back(std::move(byteStart));
			sub->stackArgs.push_back(std::move(byteEnd));

			auto diff = awst::makeUInt64BinOp(
				mkEnd(), awst::UInt64BinaryOperator::Sub, mkStart(), m_loc);
			auto lenHdr = awst::makeUInt16Bytes(std::move(diff), m_loc);
			auto cat = awst::makeConcat(std::move(lenHdr), std::move(sub), m_loc);
			return awst::makeReinterpretCast(std::move(cat), resultType, m_loc);
		}
	}

	auto slice = awst::makeIntrinsicCall("substring3", resultType, m_loc);
	slice->stackArgs.push_back(std::move(base));
	slice->stackArgs.push_back(std::move(start));
	slice->stackArgs.push_back(std::move(end));
	return slice;
}

} // namespace puyasol::builder::sol_ast
