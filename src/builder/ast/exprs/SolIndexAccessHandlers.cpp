/// @file SolIndexAccessHandlers.cpp — per-shape index-access handlers.
/// toAwst dispatchers remain in SolIndexAccess.cpp.

#include "builder/ast/exprs/SolIndexAccess.h"
#include "builder/solc/SolcFacts.h"
#include "builder/eb/MappingPrefix.h"
#include "builder/codec/EvmValueCodec.h"
#include "builder/storage/named/StoragePathWalker.h"
#include "awst/NameGen.h"
#include "builder/context/ProgramAnalysis.h"
#include "builder/eb/NodeBuilder.h"
#include "builder/storage/StorageMapper.h"
#include "builder/codec/Arc4Defaults.h"
#include "builder/types/TypeMapper.h"
#include "builder/types/TypeCoercion.h"
#include "awst/WType.h"

#include <functional>

#include <libsolidity/ast/AST.h>
#include <libsolidity/ast/TypeProvider.h>

namespace puyasol::builder::sol_ast
{
using namespace solidity::frontend;
}
namespace puyasol::builder::sol_ast
{

std::shared_ptr<awst::Expression> SolIndexAccess::readElement(
	std::shared_ptr<awst::Expression> value)
{
	return codec::valueFromArc4(m_ctx.typeMapper,
		m_indexAccess.annotation().type, std::move(value), m_loc);
}

std::shared_ptr<awst::Expression> SolIndexAccess::handleDynamicArrayAccess()
{
	auto const* arrType = dynamic_cast<ArrayType const*>(
		m_indexAccess.baseExpression().annotation().type);
	auto* elemType = m_ctx.typeMapper.mapSolTypeToARC4(arrType->baseType());
	auto* arrWType = m_ctx.typeMapper.map(arrType);

	// Box key: a box-backed array state var is keyed by its name; a box-keyed array REF
	// param (handle model) is keyed by the runtime bytes the caller passed, so a[i] reads
	// the CALLER's box. (Field WRITES go through tryHandleBoxedArrayElemWrite's box_replace.)
	std::shared_ptr<awst::BoxValueExpression> boxExpr;
	if (auto const* ident = SolcFacts::expressionAs<Identifier>(&m_indexAccess.baseExpression()))
	{
		std::string keyParam;
		auto const* decl = ident->annotation().referencedDeclaration;
		if (decl)
			keyParam = m_scope.bindings.mappingKeyParams.get(decl->id());
		if (!keyParam.empty())
		{
			auto key = awst::makeReinterpretCast(
				awst::makeVarExpression(keyParam, awst::WType::bytesType(), m_loc),
				awst::WType::boxKeyType(), m_loc);
			boxExpr = awst::makeBoxValueExpression(std::move(key), arrWType, m_loc);
		}
		else
		{
			// Same physical-binding key the writers (push/pop, dispatch) use;
			// raw source names diverge for colliding inherited declarations.
			auto boxName = ident->name();
			if (auto const* stateVar = dynamic_cast<VariableDeclaration const*>(decl);
				stateVar && stateVar->isStateVariable())
				boxName = m_ctx.storageMapper.physicalBindingFor(*stateVar).key;
			boxExpr = builder::StorageMapper::makeTopLevelBoxExpr(boxName, arrWType, m_loc);
		}
	}
	else
		boxExpr = builder::StorageMapper::makeTopLevelBoxExpr(std::string(), arrWType, m_loc);

	std::shared_ptr<awst::Expression> baseExprForRead = boxExpr;
	if (!m_indexAccess.annotation().willBeWrittenTo)
		baseExprForRead = builder::StorageMapper::makeStateGetWithDefault(boxExpr, arrWType, m_loc);

	// Index → uint64 with an out-of-bounds pre-check (a wide index >= 2^64 reverts instead of
	// silently truncating its high bits and reading arr[low-64-bits]).
	auto idx = builder::TypeCoercion::checkedIndexToUint64(
		m_ctx.preEffects(), buildExpr(*m_indexAccess.indexExpression()), m_loc);

	// DYNAMIC-element box arrays (struct-with-mapping elements → the mapping
	// member maps to dynamic bytes): puya's IndexExpression reads the uint16
	// offset table with NO length check, so an OOB index dereferences whatever
	// bytes sit at the phantom table slot — garbage instead of a revert.
	// (Static-stride elements at least die on the physical box_extract
	// boundary.) Assert idx < length, EVM Panic 0x32 semantics; length via
	// makeBoxArrayLength so it can never disagree with `.length`/push/pop.
	// Covers reads AND the write lvalue (both built here). The length helper
	// accepts either a physical state key or a storage-ref parameter's runtime
	// key, so both representations get the same recursive-shape bounds rule.
	if (arrType->isDynamicallySized() && !arrType->isByteArrayOrString()
		&& !builder::computeEncodedElementSize(elemType).fixedBytes())
		if (auto const* ident = SolcFacts::expressionAs<Identifier>(&m_indexAccess.baseExpression()))
			if (auto const* decl = dynamic_cast<VariableDeclaration const*>(
					ident->annotation().referencedDeclaration); decl)
			{
				std::shared_ptr<awst::Expression> length;
				auto const& keyParam = m_scope.bindings.mappingKeyParams.get(decl->id());
				if (!keyParam.empty())
					length = StorageMapper::makeBoxArrayLength(
						m_ctx.typeMapper,
						awst::makeReinterpretCast(
							awst::makeVarExpression(
								keyParam, awst::WType::bytesType(), m_loc),
							awst::WType::boxKeyType(), m_loc),
						m_loc);
				else if (decl->isStateVariable() && !decl->isConstant()
					&& !decl->immutable())
					length = StorageMapper::makeBoxArrayLength(m_ctx.typeMapper,
						awst::makeUtf8BytesConstant(m_ctx.storageMapper.physicalBindingFor(*decl).key,
							m_loc, awst::WType::boxKeyType()), m_loc);
				if (!length)
					return awst::makeZero(m_loc);
				// idx feeds the assert AND the element access — pin once.
				std::string tmpName = "__sol_dynix_" + std::to_string(
					awst::NameGen::next("SolIndexAccess.dynamicIndex"));
				auto tmpVar = [&]() {
					return awst::makeVarExpression(
						tmpName, awst::WType::uint64Type(), m_loc);
				};
				m_ctx.preEffects().push_back(
					awst::makeAssignmentStatement(tmpVar(), std::move(idx), m_loc));
				auto cmp = awst::makeNumericCompare(
					tmpVar(), awst::NumericComparison::Lt,
					std::move(length),
					m_loc);
				m_ctx.preEffects().push_back(awst::makeExpressionStatement(
					awst::makeAssert(std::move(cmp), m_loc, "array index out of bounds"),
					m_loc));
				idx = tmpVar();
			}

	// puya evaluates the index twice (bounds check + access); a side-effecting
	// index `arr[f()]` ran f() twice (verified cnt==2). makeEvalOnce prevents it.
	// Write path returns a bare lvalue — keep the tree assignable by pinning a
	// side-effecting index to a TEMP VAR instead (T2: `arr[f()] += 1` escaped).
	if (!m_indexAccess.annotation().willBeWrittenTo)
		idx = awst::makeEvalOnce(std::move(idx), m_loc);
	else if (dynamic_cast<awst::SubroutineCallExpression const*>(idx.get())
		|| dynamic_cast<awst::AssignmentExpression const*>(idx.get()))
	{
		std::string tempName = "__sol_widx_" + std::to_string(
			awst::NameGen::next("SolIndexAccessHandlers.writeIdxCounter"));
		auto tempVar = awst::makeVarExpression(tempName, idx->wtype, m_loc);
		m_ctx.preEffects().push_back(
			awst::makeAssignmentStatement(tempVar, std::move(idx), m_loc));
		idx = tempVar;
	}

	// bytes/string storage: puya rejects IndexExpression on bytes; use extract3.
	// Write path unsupported (needs replace3-based lvalue handler).
	if (arrType->isByteArrayOrString() && !m_indexAccess.annotation().willBeWrittenTo)
	{
		auto one = awst::makeOne(m_loc);
		return awst::makeExtract3(
			baseExprForRead, std::move(idx), std::move(one), m_loc,
			m_ctx.typeMapper.createType<awst::BytesWType>(1));
	}

	auto indexExpr = awst::makeIndexExpression(m_indexAccess.annotation().willBeWrittenTo ? boxExpr : baseExprForRead, std::move(idx), elemType, m_loc);

	if (m_indexAccess.annotation().willBeWrittenTo)
		return indexExpr;

	return readElement(std::move(indexExpr));
}

std::shared_ptr<awst::Expression> SolIndexAccess::handleMappingAccess()
{
	auto const* baseType = m_indexAccess.baseExpression().annotation().type;

	std::vector<Expression const*> indexExprs;
	Expression const* cursor = &m_indexAccess;

	while (auto const* idxAccess = SolcFacts::expressionAs<IndexAccess>(cursor))
	{
		if (idxAccess->indexExpression())
			indexExprs.push_back(idxAccess->indexExpression());
		cursor = &SolcFacts::unparenthesized(idxAccess->baseExpression());
	}
	// `(m = m2)[k]`: emit the assignment (side effect: update storageAliases),
	// then resolve from the RHS. Also peel parenthesised TupleExpression wrappers.
	while (true)
	{
		if (auto const* assign = SolcFacts::expressionAs<Assignment>(cursor))
		{
			buildExpr(*assign);
			cursor = &SolcFacts::unparenthesized(assign->rightHandSide());
			continue;
		}
		break;
	}

	auto holder = resolveStorageHolder(m_ctx, m_scope, *cursor, m_loc);
	if (!holder.key)
		throw SizeError("mapping access requires a resolved storage holder");
	auto const* rootMappingType = cursor->annotation().type;

	std::reverse(indexExprs.begin(), indexExprs.end());

	auto e = std::make_shared<awst::BoxValueExpression>();
	e->sourceLocation = m_loc;
	e->wtype = resolveValueWType(baseType);

	if (!indexExprs.empty())
	{
		// A function-returned/otherwise computed storage prefix participates in
		// both bounds checks and key derivation. Evaluate it once before walking
		// the recursive container type.
		if (!dynamic_cast<awst::VarExpression const*>(holder.key.get())
			&& !dynamic_cast<awst::BytesConstant const*>(holder.key.get()))
		{
			std::string name = "__sol_prefix_" + std::to_string(
				awst::NameGen::next("SolIndexAccessHandlers.prefixTempCounter"));
			auto const* prefixWType = holder.key->wtype;
			m_ctx.preEffects().push_back(awst::makeAssignmentStatement(
				awst::makeVarExpression(name, prefixWType, m_loc),
				std::move(holder.key), m_loc));
			holder.key = awst::makeVarExpression(name, prefixWType, m_loc);
		}

		// ARRAY levels in the chain (mapping(K=>V)[] a → a[i][k]) fold the element
		// index into the derived box key. The walker keeps the serialized array
		// value alongside the logical key so dynamic bounds read that level's
		// current value: nested arrays are encoded inside their parent box, and
		// a derived holder is an identity for descendant mapping boxes, not a
		// standalone box holding the nested length. Identical for state roots,
		// mapping values, aliases, and box-keyed storage-ref parameters.
		if (!dynamic_cast<ArrayType const*>(rootMappingType))
			holder.value = nullptr;
		StoragePathWalker walker(
			m_ctx.typeMapper, rootMappingType, m_loc, StoragePathWalker::ValueTracking::NestedArrays);
		for (auto const* indexExpr: indexExprs)
		{
			auto index = buildExpr(*indexExpr);
			holder = walker.step(std::move(holder), std::move(index), m_ctx.preEffects());
		}
	}
	e->key = std::move(holder.key);

	if (m_indexAccess.annotation().willBeWrittenTo)
		return e;

	return builder::StorageMapper::makeStateGetWithDefault(e, e->wtype, m_loc);
}

awst::WType const* SolIndexAccess::resolveValueWType(solidity::frontend::Type const* _baseType)
{
	if (auto const* mappingType = dynamic_cast<MappingType const*>(_baseType))
	{
		Type const* vt = mappingType->valueType();
		while (auto const* nested = dynamic_cast<MappingType const*>(vt))
			vt = nested->valueType();
		return m_ctx.typeMapper.map(vt);
	}
	return m_ctx.typeMapper.map(m_indexAccess.annotation().type);
}

std::shared_ptr<awst::Expression> SolIndexAccess::handleRegularIndex()
{
	// Multi-box array (>32KB): split across `<name>` ++ `itob(page)` boxes.
	// Standard IndexExpression would box_extract a non-existent single box.
	if (auto const* ident = SolcFacts::expressionAs<Identifier>(&m_indexAccess.baseExpression()))
	{
		auto const* varDecl = dynamic_cast<VariableDeclaration const*>(
			ident->annotation().referencedDeclaration);
		if (varDecl && varDecl->isStateVariable() && !varDecl->isConstant() && !varDecl->immutable()
			&& !m_indexAccess.annotation().willBeWrittenTo
			// `return m[i];` from a storage-ref pointer function wants the
			// LOCATION, not the element: FunctionBuilder rewrites that return to
			// the bare uint64 index. Materialising the element here left the
			// rewrite nothing to match, so the function silently returned a
			// struct while declared uint64 ("invalid return type
			// [PrimitiveIRType.bytes], expected [PrimitiveIRType.uint64]").
			// The call site re-indexes the state var and pages in from there.
			&& !m_ctx.typeMapper.analysis().storageRefPointerReturnAccesses
					.count(m_indexAccess.id()))
		{
			// Read context only — ResolvedLValue writes with box_replace at
			// the resolved page/offset. A ReinterpretCast cannot be a valid
			// Lvalue in puya, so we never return one here.
			auto* baseWtype = m_ctx.typeMapper.map(varDecl->type());
			if (builder::StorageMapper::isMultiBoxArray(baseWtype))
			{
				auto idxExpr = m_indexAccess.indexExpression()
					? buildExpr(*m_indexAccess.indexExpression()) : nullptr;
				if (idxExpr)
					return buildMultiBoxAccess(
						m_ctx.storageMapper.physicalBindingFor(*varDecl).key,
						baseWtype, std::move(idxExpr));
			}
		}
	}

	auto base = buildExpr(m_indexAccess.baseExpression());
	std::shared_ptr<awst::Expression> index;
	if (m_indexAccess.indexExpression())
		index = buildExpr(*m_indexAccess.indexExpression());

	// Pin a side-effecting index ONCE, before any consumer. Two independent
	// consumers share this subtree: the sol-eb dispatch coerces it through
	// checkedIndexToUint64 (emitting a pin + bounds assert that EVALUATE it)
	// and, when the builder does not claim the access, the fallthrough uses
	// the ORIGINAL subtree again — `result[--p] = 0x3d` decremented twice
	// per statement (the no-asm Base64 encoder wrote its padding into the
	// wrong cells). Pure indexes pass through: single-use temps are
	// copy-propagated by the backend.
	{
		auto triviallyPureIx = [](awst::Expression const* e) -> bool {
			while (auto const* rc = dynamic_cast<awst::ReinterpretCast const*>(e))
				e = rc->expr.get();
			return !e || dynamic_cast<awst::VarExpression const*>(e)
				|| dynamic_cast<awst::IntegerConstant const*>(e);
		};
		if (index && !triviallyPureIx(index.get()))
		{
			std::string nm = "__sol_ixpin_" + std::to_string(
				awst::NameGen::next("SolIndexAccess.indexPin"));
			auto tmp = awst::makeVarExpression(nm, index->wtype, m_loc);
			m_ctx.preEffects().push_back(
				awst::makeAssignmentStatement(tmp, std::move(index), m_loc));
			index = awst::makeVarExpression(nm, tmp->wtype, m_loc);
		}
	}

	// Try sol-eb builder dispatch
	if (index)
	{
		auto* baseSolType = m_indexAccess.baseExpression().annotation().type;
		auto baseBuilder = m_ctx.builderForInstance(baseSolType, base);
		if (baseBuilder)
		{
			auto* idxSolType = m_indexAccess.indexExpression()
				? m_indexAccess.indexExpression()->annotation().type : nullptr;
			auto idxBuilder = m_ctx.builderForInstance(idxSolType, index);
			if (!idxBuilder)
			{
				auto idxExpr = index;
				if (idxExpr->wtype == awst::WType::biguintType())
					idxExpr = builder::TypeCoercion::checkedIndexToUint64(
						m_ctx.preEffects(), std::move(idxExpr), m_loc);
				idxBuilder = m_ctx.builderForInstance(
					TypeProvider::uint256(), idxExpr);
			}
			if (idxBuilder)
			{
				auto result = baseBuilder->index(*idxBuilder, m_loc);
				if (result)
					// Write: bare lvalue (no sign-ext); read: rvalue (sign-extended).
					return m_indexAccess.annotation().willBeWrittenTo
						? result->resolve_lvalue()
						: result->resolve();
			}
		}
	}

	if (index)
		index = TypeCoercion::checkedIndexToUint64(m_ctx.preEffects(), std::move(index), m_loc);

	// Bytes reads use extract3; the writable byte view is consumed by
	// ResolvedLValue's replace3 store, not emitted as an array operation.
	if (base->wtype
		&& (base->wtype == awst::WType::bytesType()
			|| base->wtype->kind() == awst::WTypeKind::Bytes)
		&& index)
	{
		auto* bytes1Type = m_ctx.typeMapper.createType<awst::BytesWType>(1);
		if (m_indexAccess.annotation().willBeWrittenTo)
			return awst::makeIndexExpression(std::move(base), std::move(index), bytes1Type, m_loc);
		auto one = awst::makeOne(m_loc);
		return awst::makeExtract3(
			std::move(base), std::move(index), std::move(one), m_loc, bytes1Type);
	}

	auto const* elementType = awst::arrayElementType(base->wtype);
	if (!elementType) throw std::logic_error("Index receiver has no array element type");
	auto value = awst::makeIndexExpression(std::move(base), std::move(index), elementType, m_loc);
	if (!m_indexAccess.annotation().willBeWrittenTo)
		return readElement(std::move(value));
	return value;
}

std::shared_ptr<awst::Expression> SolIndexAccess::buildMultiBoxAccess(
	std::string const& _varName,
	awst::WType const* _arrWtype,
	std::shared_ptr<awst::Expression> _idxExpr)
{
	using ::puyasol::builder::StorageMapper;
	auto page = StorageMapper::arrayPageForIndex(
		_varName, _arrWtype, std::move(_idxExpr), m_ctx.preEffects(), m_loc);
	auto cast = StorageMapper::makeBoxWindowRead(
		m_ctx.typeMapper, page.key, page.offset, page.elementType, m_loc);
	return readElement(std::move(cast));
}

std::shared_ptr<awst::Expression> SolIndexAccess::handleSlicedIndex()
{
	auto slice = SolIndexRangeAccess::resolveSlice(m_ctx, m_indexAccess.baseExpression(), m_loc);
	if (!slice) return nullptr;
	auto index = m_ctx.pinIfWriteBacks(m_ctx.lower(*m_indexAccess.indexExpression(), false), m_loc);
	index = m_ctx.emitSequencedOperand({}, TypeCoercion::checkedIndexToUint64(
		m_ctx.preEffects(), std::move(index), m_loc), true, m_loc);
	m_ctx.queuePreExpression(awst::makeAssert(awst::makeNumericCompare(
		index, awst::NumericComparison::Lt, slice->length, m_loc), m_loc,
		"slice index out of bounds"), m_loc);
	auto effective = awst::makeUInt64BinOp(
		slice->offset, awst::UInt64BinaryOperator::Add, index, m_loc);
	auto value = awst::makeIndexExpression(slice->base, std::move(effective),
		awst::arrayElementType(slice->base->wtype), m_loc);
	return readElement(std::move(value));
}


} // namespace puyasol::builder::sol_ast
