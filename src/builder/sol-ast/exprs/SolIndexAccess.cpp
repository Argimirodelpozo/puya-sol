/// @file SolIndexAccess.cpp
/// Migrated from IndexAccessBuilder.cpp.

#include "builder/sol-ast/exprs/SolIndexAccess.h"
#include "builder/sol-eb/NodeBuilder.h"
#include "builder/storage/StorageMapper.h"
#include "builder/sol-types/TypeMapper.h"
#include "builder/sol-types/TypeCoercion.h"
#include "awst/WType.h"

#include <libsolidity/ast/AST.h>
#include <libsolidity/ast/TypeProvider.h>

namespace puyasol::builder::sol_ast
{

using namespace solidity::frontend;

SolIndexAccess::SolIndexAccess(eb::BuilderContext& _ctx, IndexAccess const& _node)
	: SolExpression(_ctx, _node), m_indexAccess(_node)
{
}

std::shared_ptr<awst::Expression> SolIndexAccess::toAwst()
{
	auto const* baseType = m_indexAccess.baseExpression().annotation().type;

	// Calldata/memory slice indexing: `root[a:b]...[i]`. We fold the slice
	// chain into a direct index on the root array; the bytes-substring3 path
	// would drop the element type and produce bytes[1] instead of the
	// declared element (uint256 etc).
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

	// Slot-based storage reference: _x[i] → __storage_read(slot + i)
	// For multi-dim: _x[i][j] → __storage_read(slot + i * stride + j)
	if (auto const* ident = dynamic_cast<Identifier const*>(&m_indexAccess.baseExpression()))
	{
		if (auto const* varDecl = dynamic_cast<VariableDeclaration const*>(
				ident->annotation().referencedDeclaration))
		{
			auto slotRef = m_ctx.slotStorageRefs.count(varDecl->id())
				? m_ctx.slotStorageRefs.at(varDecl->id()) : nullptr;
			if (slotRef)
			{
				// Compute slot offset from index
				auto indexExpr = m_indexAccess.indexExpression()
					? buildExpr(*m_indexAccess.indexExpression()) : nullptr;

				// Ensure index is biguint for slot arithmetic
				if (indexExpr && indexExpr->wtype == awst::WType::uint64Type())
				{
					auto itob = awst::makeIntrinsicCall("itob", awst::WType::bytesType(), m_loc);
					itob->stackArgs.push_back(std::move(indexExpr));
					auto cast = awst::makeReinterpretCast(std::move(itob), awst::WType::biguintType(), m_loc);
					indexExpr = std::move(cast);
				}

				// slot_var holds the base slot (biguint)
				auto slotVar = awst::makeVarExpression(varDecl->name(), awst::WType::biguintType(), m_loc);

				// For now, handle simple single-index: slot + index
				// The outer array is T[N][1], so _x[0] accesses the inner T[N]
				// at base slot. Each element is 1 slot (uint256).
				// For _x[0], the inner array starts at slot. We need to return
				// a "slot reference" that subsequent indexing can use.
				// For nested _x[0][j], compute slot + j.

				// If this is the outer index of a storage array (e.g., _x[0] in _x[0] = y),
				// the result should be another slot ref pointing to (slot + index * inner_size).
				// For now, just pass through the slot expression — the inner index will add j.

				auto const* arrType = dynamic_cast<ArrayType const*>(baseType);
				if (arrType && arrType->baseType()->category() == Type::Category::Array)
				{
					// Outer dimension: _x[i] → returns a "slot ref" for the inner array
					// Inner stride = inner array length
					auto const* innerArr = dynamic_cast<ArrayType const*>(arrType->baseType());
					if (innerArr && indexExpr)
					{
						unsigned innerLen = innerArr->isDynamicallySized() ? 0
							: static_cast<unsigned>(innerArr->length());
						if (innerLen > 0)
						{
							// newSlot = slot + index * innerLen
							auto stride = awst::makeIntegerConstant(std::to_string(innerLen), m_loc, awst::WType::biguintType());

							auto mul = awst::makeBigUIntBinOp(std::move(indexExpr), awst::BigUIntBinaryOperator::Mult, std::move(stride), m_loc);

							auto add = awst::makeBigUIntBinOp(std::move(slotVar), awst::BigUIntBinaryOperator::Add, std::move(mul), m_loc);
							return add;
						}
					}
					// Fallback: just return slot
					return slotVar;
				}

				// Inner dimension: _x[j] where _x is already a slot offset (biguint)
				// → __storage_read(slot + j)
				if (indexExpr)
				{
					// computedSlot = base + j
					auto add = awst::makeBigUIntBinOp(std::move(slotVar), awst::BigUIntBinaryOperator::Add, std::move(indexExpr), m_loc);

					// __storage_read expects uint64 slot, but we have biguint.
					// Truncate: btoi(add)
					auto castToBytes = awst::makeReinterpretCast(std::move(add), awst::WType::bytesType(), m_loc);

					// Safe truncate biguint to uint64: extract last 8 bytes then btoi
					auto lenOp = awst::makeIntrinsicCall("len", awst::WType::uint64Type(), m_loc);
					lenOp->stackArgs.push_back(castToBytes);
					auto sub8 = std::make_shared<awst::UInt64BinaryOperation>();
					sub8->sourceLocation = m_loc;
					sub8->wtype = awst::WType::uint64Type();
					sub8->left = std::move(lenOp);
					sub8->op = awst::UInt64BinaryOperator::Sub;
					auto eight = awst::makeIntegerConstant("8", m_loc);
					sub8->right = std::move(eight);
					auto last8 = awst::makeIntrinsicCall("extract3", awst::WType::bytesType(), m_loc);
					last8->stackArgs.push_back(std::move(castToBytes));
					last8->stackArgs.push_back(std::move(sub8));
					auto eight2 = awst::makeIntegerConstant("8", m_loc);
					last8->stackArgs.push_back(std::move(eight2));
					auto btoi = awst::makeIntrinsicCall("btoi", awst::WType::uint64Type(), m_loc);
					btoi->stackArgs.push_back(std::move(last8));

					auto call = std::make_shared<awst::SubroutineCallExpression>();
					call->sourceLocation = m_loc;
					call->wtype = awst::WType::biguintType();
					call->target = awst::InstanceMethodTarget{"__storage_read"};
					awst::CallArg arg;
					arg.name = "__slot";
					arg.value = std::move(btoi);
					call->args.push_back(std::move(arg));
					return call;
				}
			}
		}
	}

	// Slot-based storage index: if the base resolves to biguint AND the
	// Solidity type is a storage-located array, treat as slot arithmetic.
	// This handles any expression chain: _x[i][j], getArray()[j], etc.
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
					// Ensure index is biguint
					if (indexExpr->wtype == awst::WType::uint64Type())
					{
						auto itob = awst::makeIntrinsicCall("itob", awst::WType::bytesType(), m_loc);
						itob->stackArgs.push_back(std::move(indexExpr));
						auto cast = awst::makeReinterpretCast(std::move(itob), awst::WType::biguintType(), m_loc);
						indexExpr = std::move(cast);
					}

					auto add = awst::makeBigUIntBinOp(std::move(baseExpr), awst::BigUIntBinaryOperator::Add, std::move(indexExpr), m_loc);

					// Read: __storage_read(truncated_slot)
					if (!m_indexAccess.annotation().willBeWrittenTo)
					{
						auto castToBytes = awst::makeReinterpretCast(std::move(add), awst::WType::bytesType(), m_loc);

						// Safe truncate biguint to uint64
						auto last8 = awst::makeIntrinsicCall("extract", awst::WType::bytesType(), m_loc);
						last8->immediates = {24, 8};
						last8->stackArgs.push_back(std::move(castToBytes));

						auto btoi = awst::makeIntrinsicCall("btoi", awst::WType::uint64Type(), m_loc);
						btoi->stackArgs.push_back(std::move(last8));

						auto call = std::make_shared<awst::SubroutineCallExpression>();
						call->sourceLocation = m_loc;
						call->wtype = awst::WType::biguintType();
						call->target = awst::InstanceMethodTarget{"__storage_read"};
						awst::CallArg arg;
						arg.name = "__slot";
						arg.value = std::move(btoi);
						call->args.push_back(std::move(arg));
						return call;
					}
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

	return handleRegularIndex();
}

// ── IndexRangeAccess ──

SolIndexRangeAccess::SolIndexRangeAccess(
	eb::BuilderContext& _ctx, IndexRangeAccess const& _node)
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
		auto zero = awst::makeIntegerConstant("0", m_loc);
		start = std::move(zero);
	}

	std::shared_ptr<awst::Expression> end;
	if (m_rangeAccess.endExpression())
		end = buildExpr(*m_rangeAccess.endExpression());
	else
	{
		// Default end for substring3: byte-count via `len` intrinsic,
		// preserving pre-existing full-slice semantics.
		auto lenCall = awst::makeIntrinsicCall("len", awst::WType::uint64Type(), m_loc);
		lenCall->stackArgs.push_back(base);
		end = std::move(lenCall);
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
			auto lenNode = std::make_shared<awst::ArrayLength>();
			lenNode->sourceLocation = m_loc;
			lenNode->wtype = awst::WType::uint64Type();
			lenNode->array = base;
			lenExpr = std::move(lenNode);
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

		int elemSize = elemType ? builder::TypeCoercion::computeEncodedElementSize(elemType) : 0;

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
					awst::makeIntegerConstant(std::to_string(elemSize), m_loc),
					m_loc);
				if (headerBytes > 0)
				{
					return awst::makeUInt64BinOp(
						std::move(scale),
						awst::UInt64BinaryOperator::Add,
						awst::makeIntegerConstant(std::to_string(headerBytes), m_loc),
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
			auto itob = awst::makeIntrinsicCall("itob", awst::WType::bytesType(), m_loc);
			itob->stackArgs.push_back(std::move(diff));

			auto lenHdr = awst::makeIntrinsicCall("extract3", awst::WType::bytesType(), m_loc);
			lenHdr->stackArgs.push_back(std::move(itob));
			lenHdr->stackArgs.push_back(awst::makeIntegerConstant("6", m_loc));
			lenHdr->stackArgs.push_back(awst::makeIntegerConstant("2", m_loc));

			auto cat = awst::makeIntrinsicCall("concat", awst::WType::bytesType(), m_loc);
			cat->stackArgs.push_back(std::move(lenHdr));
			cat->stackArgs.push_back(std::move(sub));

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
