/// @file SolIndexAccess.cpp
/// Migrated from IndexAccessBuilder.cpp.

#include "builder/sol-ast/exprs/SolIndexAccess.h"
#include "builder/sol-eb/NodeBuilder.h"
#include "builder/storage/StorageMapper.h"
#include "builder/sol-types/TypeMapper.h"
#include "builder/sol-types/Arc4Defaults.h"
#include "builder/sol-types/TypeCoercion.h"
#include "builder/assembly/AssemblyBuilder.h"
#include "awst/WType.h"

#include <libsolidity/ast/AST.h>
#include <libsolidity/ast/TypeProvider.h>

namespace puyasol::builder::sol_ast
{

using namespace solidity::frontend;

namespace
{
// Truncate a biguint storage slot to its low 8 bytes and read it via
// __puyasol___storage_read. Slots are full-width (sha256-derived), so last-8
// matches both prior call sites (one used extractLastN, one extract(24,8)).
std::shared_ptr<awst::Expression> readStorageSlotBiguint(
	std::shared_ptr<awst::Expression> _slot, awst::SourceLocation const& _loc)
{
	auto last8 = awst::makeExtractLastN(awst::makeAsBytes(std::move(_slot), _loc), 8, _loc);
	auto call = awst::makeSubroutineCall(
		awst::SubroutineID{"__puyasol___storage_read"}, awst::WType::biguintType(), _loc);
	awst::pushCallArg(call->args, "__slot", awst::makeBtoi(std::move(last8), _loc));
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
				if (arrType && arrType->baseType()->category() == Type::Category::Array)
				{
					// Outer dim: slot ref for the inner array (stride = inner array length).
					auto const* innerArr = dynamic_cast<ArrayType const*>(arrType->baseType());
					if (innerArr && indexExpr)
					{
						unsigned innerLen = innerArr->isDynamicallySized() ? 0
							: static_cast<unsigned>(innerArr->length());
						if (innerLen > 0)
						{
							// newSlot = slot + index * innerLen
							auto stride = awst::makeIntegerConstant(innerLen, m_loc, awst::WType::biguintType());

							auto mul = awst::makeBigUIntBinOp(std::move(indexExpr), awst::BigUIntBinaryOperator::Mult, std::move(stride), m_loc);

							auto add = awst::makeBigUIntBinOp(std::move(slotVar), awst::BigUIntBinaryOperator::Add, std::move(mul), m_loc);
							return add;
						}
					}
					// Fallback: just return slot
					return slotVar;
				}

				// Inner dim: _x is already a biguint slot offset → __storage_read(slot+j)
				if (indexExpr)
				{
					auto add = awst::makeBigUIntBinOp(std::move(slotVar), awst::BigUIntBinaryOperator::Add, std::move(indexExpr), m_loc);
					return readStorageSlotBiguint(std::move(add), m_loc);
				}
			}
		}
	}

	// Slot arithmetic for any biguint base on a storage-located array
	// (_x[i][j], getArray()[j], etc.).
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

					auto add = awst::makeBigUIntBinOp(std::move(baseExpr), awst::BigUIntBinaryOperator::Add, std::move(indexExpr), m_loc);

					if (!m_indexAccess.annotation().willBeWrittenTo) // read: __storage_read(truncated_slot)
						return readStorageSlotBiguint(std::move(add), m_loc);
					// Write: return computed slot for assignment handler
					return add;
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
		return awst::makeUInt64BinOp(std::move(parent), awst::UInt64BinaryOperator::Add,
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
	auto base = buildExpr(m_rangeAccess.baseExpression());

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
