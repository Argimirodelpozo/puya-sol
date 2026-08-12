/// @file SolIndexAccess.cpp
/// Migrated from IndexAccessBuilder.cpp.

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

std::shared_ptr<awst::Expression> SolIndexAccess::materializeSlotArray(
	std::shared_ptr<awst::Expression> _baseSlot,
	solidity::frontend::ArrayType const* _arrType)
{
	if (!_arrType || _arrType->isDynamicallySized())
		return nullptr;
	auto lenU = _arrType->length();
	if (lenU == 0 || lenU > 64)
	{
		Logger::instance().error(
			"cannot materialize slot-handle array of length " + lenU.str()
			+ " (unrolled reads capped at 64 elements)", m_loc);
		return nullptr;
	}
	unsigned len = static_cast<unsigned>(lenU);
	auto const* elemType = _arrType->baseType();
	auto const* arrW = m_ctx.typeMapper.map(_arrType);
	auto arr = awst::makeNewArray(arrW, m_loc);

	// bind the base slot once (read per element)
	std::string tmp = "__slotarr_" + std::to_string(m_indexAccess.id());
	m_ctx.prePendingStatements.push_back(awst::makeAssignmentStatement(
		awst::makeVarExpression(tmp, awst::WType::biguintType(), m_loc),
		std::move(_baseSlot), m_loc));
	auto baseVar = [&]() {
		return awst::makeVarExpression(tmp, awst::WType::biguintType(), m_loc);
	};

	if (auto const* structElem = dynamic_cast<StructType const*>(elemType))
	{
		auto const* structW = dynamic_cast<awst::ARC4Struct const*>(
			m_ctx.typeMapper.map(structElem));
		if (!structW)
			return nullptr;
		auto stride = structElem->storageSize();
		for (unsigned j = 0; j < len; ++j)
		{
			auto elemBase = awst::makeBigUIntBinOp(baseVar(),
				awst::BigUIntBinaryOperator::Add,
				awst::makeIntegerConstant((stride * j).str(), m_loc, awst::WType::biguintType()),
				m_loc);
			arr->values.push_back(builder::SlotHandleAccess::readStructElem(
				m_ctx.prePendingStatements, std::move(elemBase), structElem, structW, m_loc));
		}
		return arr;
	}

	// scalar elements → canonical biguint reads, ARC4-encoded per element
	awst::WType const* elemW = nullptr;
	if (auto const* sa = dynamic_cast<awst::ARC4StaticArray const*>(arrW))
		elemW = sa->elementType();
	if (!elemW)
		elemW = m_ctx.typeMapper.map(elemType);
	auto layout = builder::SlotHandleAccess::layoutFor(elemType);
	for (unsigned j = 0; j < len; ++j)
	{
		auto v = builder::SlotHandleAccess::readScalarElem(
			baseVar(), awst::makeIntegerConstant(j, m_loc, awst::WType::biguintType()),
			layout, elemType, m_loc);
		if (elemW == awst::WType::arc4BoolType())
		{
			auto b = awst::makeNumericCompare(std::move(v), awst::NumericComparison::Ne,
				awst::makeIntegerConstant("0", m_loc, awst::WType::biguintType()), m_loc);
			arr->values.push_back(awst::makeARC4Encode(std::move(b), elemW, m_loc));
		}
		else
			arr->values.push_back(awst::makeARC4Encode(std::move(v), elemW, m_loc));
	}
	return arr;
}

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
	if (builder::evmStorageLayout()
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
			m_ctx.queuePrePending(awst::makeAssignmentStatement(
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
					m_ctx.queuePrePending(std::move(ps));
			}
			idx = awst::makeEvalOnce(std::move(idx), m_loc);
			auto inBounds = awst::makeNumericCompare(idx,
				awst::NumericComparison::Lt, awst::makeLen(wv(), m_loc), m_loc);
			m_ctx.queuePrePending(awst::makeExpressionStatement(
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
		if (resType && resType->isValueType())
			return low.readValue(*addr);
		if (EvmSlotLowering::isBytesLike(resType))
			return low.readBytesValue(*addr);
		if (dynamic_cast<StructType const*>(resType))
			return low.readStructValue(*addr);
		if (auto const* rat = dynamic_cast<ArrayType const*>(resType);
			rat && !rat->isByteArrayOrString())
			return low.readArrayValue(*addr, rat);
		Logger::instance().error(
			"--evm-storage-layout: aggregate storage element used as a value "
			"is not yet supported", m_loc);
		return nullptr;
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

	// Slot-based storage: _x[i] → __storage_read(slot+i);
	// multi-dim: _x[i][j] → __storage_read(slot + i*stride + j)
	if (auto const* ident = dynamic_cast<Identifier const*>(&m_indexAccess.baseExpression()))
	{
		if (auto const* varDecl = dynamic_cast<VariableDeclaration const*>(
				ident->annotation().referencedDeclaration))
		{
			auto slotRef = m_scope.findSlotStorageRef(varDecl->id());
			if (slotRef)
			{
				auto indexExpr = m_indexAccess.indexExpression()
					? buildExpr(*m_indexAccess.indexExpression()) : nullptr;

				if (indexExpr && indexExpr->wtype == awst::WType::uint64Type())
				{
					auto itob = awst::makeItob(std::move(indexExpr), m_loc);
					indexExpr = awst::makeAsBiguint(std::move(itob), m_loc);
				}

				auto slotVar = awst::makeVarExpression(varDecl->name(), awst::WType::biguintType(), m_loc);

				auto const* arrType = dynamic_cast<ArrayType const*>(baseType);
				// FIXED arrays: assert idx < length (EVM Panic 0x32) — an OOB
				// slot-handle access would silently hit a NEIGHBORING state
				// variable's slot. Also pins idx for the multi-use math below.
				indexExpr = builder::SlotHandleAccess::boundsCheckIndex(
					m_ctx.prePendingStatements, std::move(indexExpr), arrType, m_loc);
				if (arrType && arrType->baseType()->category() == Type::Category::Array)
				{
					// Outer dim: slot ref for the inner array. EVM stride = the
					// inner array's SLOT footprint (storageSize), not its element
					// count — packed inner arrays span fewer slots, multislot
					// struct elements span more.
					auto const* innerArr = dynamic_cast<ArrayType const*>(arrType->baseType());
					if (innerArr && indexExpr && !innerArr->isDynamicallySized())
					{
						auto stride = awst::makeIntegerConstant(
							innerArr->storageSize().str(), m_loc, awst::WType::biguintType());
						auto mul = awst::makeBigUIntBinOp(std::move(indexExpr), awst::BigUIntBinaryOperator::Mult, std::move(stride), m_loc);
						auto add = awst::makeBigUIntBinOp(std::move(slotVar), awst::BigUIntBinaryOperator::Add, std::move(mul), m_loc);
						return add;
					}
					// Fallback: just return slot
					return slotVar;
				}

				// Inner dim: _x is a biguint slot handle. Reads are packed-aware
				// element reads; writes return the computed slot for the
				// assignment handler (packed/struct element writes intercept in
				// SolAssignment BEFORE this path builds).
				if (indexExpr)
				{
					if (m_indexAccess.annotation().willBeWrittenTo)
						return awst::makeBigUIntBinOp(std::move(slotVar),
							awst::BigUIntBinaryOperator::Add, std::move(indexExpr), m_loc);
					auto layout = builder::SlotHandleAccess::layoutFor(
						arrType ? arrType->baseType() : nullptr);
					return builder::SlotHandleAccess::readScalarElem(
						std::move(slotVar), std::move(indexExpr), layout,
						arrType ? arrType->baseType() : nullptr, m_loc);
				}
			}
		}
	}

	// Slot arithmetic for any biguint base on a storage-located array
	// (_x[i][j], getArray()[j], etc.). Result shapes:
	//   outer dim (result is an array)  → inner handle (slot), or materialized
	//                                     memory array in read position
	//   struct element                  → slot (write) / NewStruct (read)
	//   scalar element                  → slot (write) / packed-aware read
	{
		auto const* baseSolType = m_indexAccess.baseExpression().annotation().type;
		auto const* baseArrayType = dynamic_cast<ArrayType const*>(baseSolType);
		if (baseArrayType && baseArrayType->dataStoredIn(DataLocation::Storage)
			&& m_indexAccess.indexExpression())
		{
			auto baseExpr = buildExpr(m_indexAccess.baseExpression());
			if (baseExpr && baseExpr->wtype == awst::WType::biguintType())
			{
				auto indexExpr = buildExpr(*m_indexAccess.indexExpression());
				if (indexExpr)
				{
					if (indexExpr->wtype == awst::WType::uint64Type()) // ensure biguint
					{
						auto itob = awst::makeItob(std::move(indexExpr), m_loc);
						indexExpr = awst::makeAsBiguint(std::move(itob), m_loc);
					}

					// FIXED arrays: assert idx < length (EVM Panic 0x32) —
					// OOB would silently address a neighboring slot.
					indexExpr = builder::SlotHandleAccess::boundsCheckIndex(
						m_ctx.prePendingStatements, std::move(indexExpr), baseArrayType, m_loc);

					bool written = m_indexAccess.annotation().willBeWrittenTo;
					auto const* elemType = baseArrayType->baseType();

					// Outer dim: result is itself a (fixed) array — handle = base
					// + idx * inner storageSize; reads materialize a memory copy.
					if (auto const* resArr = dynamic_cast<ArrayType const*>(elemType))
					{
						if (!resArr->isDynamicallySized())
						{
							auto stride = awst::makeIntegerConstant(
								resArr->storageSize().str(), m_loc, awst::WType::biguintType());
							auto mul = awst::makeBigUIntBinOp(std::move(indexExpr),
								awst::BigUIntBinaryOperator::Mult, std::move(stride), m_loc);
							auto add = awst::makeBigUIntBinOp(std::move(baseExpr),
								awst::BigUIntBinaryOperator::Add, std::move(mul), m_loc);
							if (written)
								return add;
							if (auto arr = materializeSlotArray(std::move(add), resArr))
								return arr;
							return nullptr;
						}
					}

					// Struct element.
					if (auto const* structElem = dynamic_cast<StructType const*>(elemType))
					{
						auto stride = awst::makeIntegerConstant(
							structElem->storageSize().str(), m_loc, awst::WType::biguintType());
						auto mul = awst::makeBigUIntBinOp(std::move(indexExpr),
							awst::BigUIntBinaryOperator::Mult, std::move(stride), m_loc);
						auto add = awst::makeBigUIntBinOp(std::move(baseExpr),
							awst::BigUIntBinaryOperator::Add, std::move(mul), m_loc);
						if (written)
							return add;
						auto const* structW = dynamic_cast<awst::ARC4Struct const*>(
							m_ctx.typeMapper.map(structElem));
						if (!structW)
							return nullptr;
						return builder::SlotHandleAccess::readStructElem(
							m_ctx.prePendingStatements, std::move(add), structElem, structW, m_loc);
					}

					// Scalar element.
					if (written)
						return awst::makeBigUIntBinOp(std::move(baseExpr),
							awst::BigUIntBinaryOperator::Add, std::move(indexExpr), m_loc);
					auto layout = builder::SlotHandleAccess::layoutFor(elemType);
					return builder::SlotHandleAccess::readScalarElem(
						std::move(baseExpr), std::move(indexExpr), layout, elemType, m_loc);
				}
			}
		}
	}

	// Box-backed dynamic array access
	bool isDynamicArrayAccess = false;
	if (auto const* arrType = dynamic_cast<ArrayType const*>(baseType))
	{
		if (auto const* ident = dynamic_cast<Identifier const*>(
				&m_indexAccess.baseExpression()))
		{
			if (auto const* varDecl = dynamic_cast<VariableDeclaration const*>(
					ident->annotation().referencedDeclaration))
			{
				if (varDecl->isStateVariable() && arrType->isDynamicallySized()
					&& !varDecl->isConstant() && !varDecl->immutable())
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

	// `base[i]` → parentOffset + i * sizeof(element-after-index).
	if (auto const* ia = dynamic_cast<IndexAccess const*>(&_node))
	{
		if (!ia->indexExpression()) return nullptr;
		auto parent = resolveBlobOffset(_ctx, _scope, ia->baseExpression(), _loc);
		if (!parent) return nullptr;
		auto idx = _ctx.buildExpr(*ia->indexExpression());
		idx = builder::TypeCoercion::implicitNumericCast(
			std::move(idx), awst::WType::uint64Type(), _loc);
		unsigned stride = builder::computeEncodedElementSize(
			_ctx.typeMapper.map(ia->annotation().type));
		// EVM memory layout: a DYNAMIC array's pointer addresses its LENGTH
		// word, so element i starts 32 bytes further in. Without this every
		// element write landed one slot early — element 0 clobbered the length
		// and the last element was never written (silent corruption, seen as
		// [22,33,0] for a [11,22,33] copy). Fixed-size arrays have no length
		// word and are already correct.
		auto const* baseArr = dynamic_cast<ArrayType const*>(
			ia->baseExpression().annotation().type);
		std::shared_ptr<awst::Expression> base = std::move(parent);
		if (baseArr && baseArr->isDynamicallySized() && !baseArr->isByteArrayOrString())
			base = awst::makeUInt64BinOp(std::move(base),
				awst::UInt64BinaryOperator::Add,
				awst::makeIntegerConstant(uint64_t{32}, _loc), _loc);
		return awst::makeUInt64BinOp(std::move(base), awst::UInt64BinaryOperator::Add,
			awst::makeUInt64BinOp(std::move(idx), awst::UInt64BinaryOperator::Mult,
				awst::makeIntegerConstant(static_cast<uint64_t>(stride), _loc), _loc), _loc);
	}

	// `base.field` → parentOffset + sum of encoded sizes of preceding members.
	if (auto const* ma = dynamic_cast<MemberAccess const*>(&_node))
	{
		auto parent = resolveBlobOffset(_ctx, _scope, ma->expression(), _loc);
		if (!parent) return nullptr;
		auto const* structType = dynamic_cast<StructType const*>(
			ma->expression().annotation().type);
		if (!structType) return nullptr;
		uint64_t fieldOff = 0;
		for (auto const& m: structType->structDefinition().members())
		{
			if (m->name() == ma->memberName()) break;
			fieldOff += static_cast<uint64_t>(
				builder::computeEncodedElementSize(_ctx.typeMapper.map(m->type())));
		}
		if (fieldOff == 0) return parent;
		return awst::makeUInt64BinOp(std::move(parent), awst::UInt64BinaryOperator::Add,
			awst::makeIntegerConstant(fieldOff, _loc), _loc);
	}

	return nullptr;
}

std::shared_ptr<awst::Expression> SolIndexAccess::readBlobValue(
	eb::ContractContext& _ctx, std::shared_ptr<awst::Expression> _off,
	solidity::frontend::Type const* _solType, awst::SourceLocation const& _loc)
{
	using namespace solidity::frontend;

	bool isAggregate = _solType
		&& (dynamic_cast<ArrayType const*>(_solType) || dynamic_cast<StructType const*>(_solType));

	// Scalar leaf (Fr / uintN / bool / address) → one 32-byte word as biguint.
	if (!isAggregate)
		return awst::makeAsBiguint(
			builder::AssemblyBuilder::readMemWordDirect(std::move(_off), _loc), _loc);

	// Struct / static-array leaf: the blob holds the ARC4 (flat ABI) encoding —
	// each field/element is a 32-byte big-endian word, exactly the ARC4 layout —
	// so reinterpreting `sz` bytes as the mapped ARC4 type yields the value.
	auto* mapped = _ctx.typeMapper.map(_solType);
	int sz = builder::computeEncodedElementSize(mapped);
	if (sz <= 0 || sz > builder::AssemblyBuilder::SLOT_SIZE)
		return nullptr;  // too large to hold as a single value — caller falls back
	return awst::makeReinterpretCast(
		builder::AssemblyBuilder::readMemRangeDirect(std::move(_off), sz, _loc), mapped, _loc);
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
	// Stash bounds in temps and emit asserts via prePendingStatements so
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
		m_ctx.prePendingStatements.push_back(
			awst::makeAssignmentStatement(startVar, start, m_loc));

		auto endVar = awst::makeVarExpression(endVarName, awst::WType::uint64Type(), m_loc);
		m_ctx.prePendingStatements.push_back(
			awst::makeAssignmentStatement(endVar, end, m_loc));

		// assert(start <= end)
		{
			auto cmp = awst::makeNumericCompare(
				awst::makeVarExpression(startVarName, awst::WType::uint64Type(), m_loc),
				awst::NumericComparison::Lte,
				awst::makeVarExpression(endVarName, awst::WType::uint64Type(), m_loc),
				m_loc);
			m_ctx.prePendingStatements.push_back(awst::makeExpressionStatement(
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
			m_ctx.prePendingStatements.push_back(awst::makeExpressionStatement(
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

		int elemSize = elemType ? builder::computeEncodedElementSize(elemType) : 0;

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
