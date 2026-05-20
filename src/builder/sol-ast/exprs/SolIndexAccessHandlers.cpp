/// @file SolIndexAccessHandlers.cpp
/// Per-shape index-access handlers extracted from SolIndexAccess.cpp:
/// dynamic-array, mapping, regular index, and sliced index. The toAwst
/// dispatchers (SolIndexAccess::toAwst and SolIndexRangeAccess::toAwst)
/// remain in SolIndexAccess.cpp.

#include "builder/sol-ast/exprs/SolIndexAccess.h"
#include "builder/sol-eb/NodeBuilder.h"
#include "builder/storage/StorageMapper.h"
#include "builder/sol-types/TypeMapper.h"
#include "builder/sol-types/TypeCoercion.h"
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

std::shared_ptr<awst::Expression> SolIndexAccess::handleDynamicArrayAccess()
{
	auto const* arrType = dynamic_cast<ArrayType const*>(
		m_indexAccess.baseExpression().annotation().type);
	auto* rawElemType = m_ctx.typeMapper.map(arrType->baseType());
	auto* elemType = m_ctx.typeMapper.mapSolTypeToARC4(arrType->baseType());
	auto* arrWType = m_ctx.typeMapper.map(arrType);

	std::string arrayVarName;
	if (auto const* ident = dynamic_cast<Identifier const*>(&m_indexAccess.baseExpression()))
		arrayVarName = ident->name();

	auto boxKey = awst::makeUtf8BytesConstant(arrayVarName, m_loc, awst::WType::boxKeyType());

	auto boxExpr = awst::makeBoxValueExpression(boxKey, arrWType, m_loc);

	std::shared_ptr<awst::Expression> baseExprForRead = boxExpr;
	if (!m_indexAccess.annotation().willBeWrittenTo)
		baseExprForRead = builder::StorageMapper::makeStateGetWithDefault(boxExpr, arrWType, m_loc);

	auto idx = buildExpr(*m_indexAccess.indexExpression());
	idx = builder::TypeCoercion::implicitNumericCast(std::move(idx), awst::WType::uint64Type(), m_loc);

	// For bytes (dynamic byte array) storage, use extract3 instead of
	// IndexExpression — puya's IR builder rejects indexing on a bytes
	// value and expects a ReferenceArray/ARC4DynamicArray shape.
	// Only applied in READ context; assignment path falls through to the
	// default IndexExpression (not yet supported — lvalue bytes indexing
	// would need a separate replace3-based handler).
	if (arrType->isByteArrayOrString() && !m_indexAccess.annotation().willBeWrittenTo)
	{
		// When reading, the base expression is the raw bytes stored in the
		// box (after stripping the ARC4 length header if any). The state
		// var is stored as raw bytes in this path, so `extract3` directly.
		auto one = awst::makeOne(m_loc);
		return awst::makeExtract3(
			baseExprForRead, std::move(idx), std::move(one), m_loc,
			m_ctx.typeMapper.createType<awst::BytesWType>(1));
	}

	auto indexExpr = awst::makeIndexExpression(m_indexAccess.annotation().willBeWrittenTo ? boxExpr : baseExprForRead, std::move(idx), elemType, m_loc);

	if (m_indexAccess.annotation().willBeWrittenTo)
		return indexExpr;

	// Only ARC4Decode if element needs decoding to native type
	bool needsDecode = rawElemType != elemType && rawElemType->name() != elemType->name();
	if (needsDecode)
	{
		auto decode = awst::makeARC4Decode(std::move(indexExpr), rawElemType, m_loc);
		return decode;
	}
	return indexExpr;
}

std::shared_ptr<awst::Expression> SolIndexAccess::handleMappingAccess()
{
	auto const* baseType = m_indexAccess.baseExpression().annotation().type;

	std::vector<Expression const*> indexExprs;
	Expression const* cursor = &m_indexAccess;
	std::string varName = "map";

	while (auto const* idxAccess = dynamic_cast<IndexAccess const*>(cursor))
	{
		if (idxAccess->indexExpression())
			indexExprs.push_back(idxAccess->indexExpression());
		cursor = &idxAccess->baseExpression();
	}
	// Peel wrappers: `(m = m2)[k]` — the assignment's value is whatever the
	// RHS points to. Realize the assignment so its side effect (updating
	// storageAliases for mapping-storage pointers) is preserved, then
	// continue resolving from the RHS. Solidity wraps parenthesised
	// expressions in TupleExpressions, so peel those too.
	while (true)
	{
		if (auto const* assign = dynamic_cast<Assignment const*>(cursor))
		{
			buildExpr(*assign);
			cursor = &assign->rightHandSide();
			continue;
		}
		if (auto const* tuple = dynamic_cast<TupleExpression const*>(cursor))
		{
			if (!tuple->isInlineArray() && tuple->components().size() == 1
				&& tuple->components()[0])
			{
				cursor = tuple->components()[0].get();
				continue;
			}
		}
		break;
	}

	auto ctx = resolveCursorContext(cursor);
	if (!ctx.varName.empty())
		varName = ctx.varName;

	std::reverse(indexExprs.begin(), indexExprs.end());

	auto declaredKeyWTypes = resolveKeyWTypes(ctx.rootMappingType, indexExprs.size());

	auto e = std::make_shared<awst::BoxValueExpression>();
	e->sourceLocation = m_loc;
	e->wtype = resolveValueWType(baseType);

	auto prefix = buildInitialPrefix(cursor, varName, ctx.aliasOverridePrefix);

	if (!indexExprs.empty())
	{
		// Per-layer hash derivation (Solidity-style): start with the initial
		// prefix and apply `sha256(keyBytes ++ currentPrefix)` per layer.
		std::shared_ptr<awst::Expression> currentPrefix = std::move(prefix);

		for (size_t ki = 0; ki < indexExprs.size(); ++ki)
		{
			auto translated = buildExpr(*indexExprs[ki]);
			awst::WType const* keyWType = (ki < declaredKeyWTypes.size() && declaredKeyWTypes[ki])
				? declaredKeyWTypes[ki] : awst::WType::uint64Type();

			if (keyWType != translated->wtype)
				translated = builder::TypeCoercion::implicitNumericCast(
					std::move(translated), keyWType, m_loc);

			if (dynamic_cast<awst::AssignmentExpression const*>(translated.get()))
			{
				static int idxTempCounter = 0;
				std::string tempName = "__sol_idx_" + std::to_string(idxTempCounter++);
				auto tempVar = awst::makeVarExpression(tempName, translated->wtype, m_loc);
				auto saveStmt = awst::makeAssignmentStatement(
					tempVar, std::move(translated), m_loc);
				m_ctx.prePendingStatements.push_back(std::move(saveStmt));
				translated = tempVar;
			}

			currentPrefix = awst::makeMappingKeyLayer(
				std::move(translated), keyWType, std::move(currentPrefix), m_loc);
		}

		e->key = std::move(currentPrefix);
	}
	else
		e->key = std::move(prefix);

	if (m_indexAccess.annotation().willBeWrittenTo)
		return e;

	return builder::StorageMapper::makeStateGetWithDefault(e, e->wtype, m_loc);
}

std::vector<awst::WType const*> SolIndexAccess::resolveKeyWTypes(
	solidity::frontend::Type const* _rootType, size_t _numLevels)
{
	std::vector<awst::WType const*> result;
	Type const* walkType = _rootType;
	for (size_t i = 0; i < _numLevels; ++i)
	{
		if (auto const* mt = dynamic_cast<MappingType const*>(walkType))
		{
			result.push_back(m_ctx.typeMapper.map(mt->keyType()));
			walkType = mt->valueType();
		}
		else
		{
			result.push_back(nullptr);
			if (auto const* at = dynamic_cast<ArrayType const*>(walkType))
				walkType = at->baseType();
			else
				break;
		}
	}
	return result;
}

SolIndexAccess::CursorContext SolIndexAccess::resolveCursorContext(
	solidity::frontend::Expression const* _cursor)
{
	CursorContext out;

	if (auto const* ident = dynamic_cast<Identifier const*>(_cursor))
	{
		out.varName = ident->name();
		out.rootMappingType = ident->annotation().type;

		// Storage pointer alias: `mapping storage m = m1; m[k] = v;`
		// The identifier resolves to a local with a registered alias;
		// the box prefix must match the underlying state variable, not
		// the local's name, or writes land under the wrong key.
		if (auto const* decl = ident->annotation().referencedDeclaration)
		{
			auto const* alias = m_scope.findStorageAlias(decl->id());
			if (alias)
			{
				auto aliasExpr = alias->expr;
				// Unwrap StateGet → underlying state expression.
				if (auto sg = std::dynamic_pointer_cast<awst::StateGet>(aliasExpr))
					aliasExpr = sg->field;
				// Peel off FieldExpressions to reach the root state expression.
				while (auto fe = std::dynamic_pointer_cast<awst::FieldExpression>(aliasExpr))
					aliasExpr = fe->base;
				// Under per-layer hashing the alias's box-key expression IS the
				// slot pointer at that level — feed it directly as the chain's
				// starting prefix. No inner-sha256 unwrapping required.
				if (auto boxVal = std::dynamic_pointer_cast<awst::BoxValueExpression>(aliasExpr))
					out.aliasOverridePrefix = boxVal->key;
				else if (auto appState = std::dynamic_pointer_cast<awst::AppStateExpression>(aliasExpr))
					out.aliasOverridePrefix = appState->key;
				// Simple holder-name alias (`mapping storage m = state_m;`):
				// alias is a BytesConstant of the underlying state-var's
				// encoded name. Use as varName so the default-prefix path picks
				// the right starting bytes.
				else if (auto const* bc = dynamic_cast<awst::BytesConstant const*>(aliasExpr.get()))
					out.varName = std::string(bc->value.begin(), bc->value.end());
			}
		}
	}
	else if (auto const* ma = dynamic_cast<MemberAccess const*>(_cursor))
	{
		out.varName = ma->memberName();
		out.rootMappingType = ma->annotation().type;
	}
	// `f()[k]` — mapping-pointer-returning call indexed directly. The call
	// result (bytes — the holder name) is the runtime key prefix.
	else if (dynamic_cast<solidity::frontend::FunctionCall const*>(_cursor))
	{
		out.rootMappingType = _cursor->annotation().type;
	}

	return out;
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

std::shared_ptr<awst::Expression> SolIndexAccess::buildInitialPrefix(
	solidity::frontend::Expression const* _cursor,
	std::string const& _varName,
	std::shared_ptr<awst::Expression> _aliasOverridePrefix)
{
	// 1. Mapping-storage-ref param: prefix is the runtime bytes value the
	//    caller passed (its state-var holder name encoded as bytes).
	if (auto const* ident = dynamic_cast<Identifier const*>(_cursor))
		if (auto const* decl = ident->annotation().referencedDeclaration)
		{
			auto const& mappingKeyParam = m_scope.findMappingKeyParam(decl->id());
			if (!mappingKeyParam.empty())
				return awst::makeVarExpression(
					mappingKeyParam, awst::WType::bytesType(), m_loc);
		}

	// 2. `f()[k]`: evaluate the call; its bytes return is the prefix.
	if (dynamic_cast<solidity::frontend::FunctionCall const*>(_cursor))
	{
		auto p = buildExpr(*_cursor);
		if (p && p->wtype != awst::WType::bytesType())
			p = builder::TypeCoercion::coerceForAssignment(
				std::move(p), awst::WType::bytesType(), m_loc);
		return p;
	}

	// 3. Alias-override prefix: the alias's box-key expression IS the
	//    slot pointer at that level (set by the cursor-resolution block
	//    above when the cursor identifier is a storage alias).
	if (_aliasOverridePrefix)
		return _aliasOverridePrefix;

	// 4. Plain state variable: utf8 bytes of the var name.
	return awst::makeUtf8BytesConstant(_varName, m_loc, awst::WType::boxKeyType());
}

std::shared_ptr<awst::Expression> SolIndexAccess::handleRegularIndex()
{
	// Multi-box state-var array: when the encoded array exceeds AVM's
	// 32KB box capacity, the storage gets split across N boxes keyed
	// `<name>` ++ `itob(page)`. Element accesses route at runtime via
	// `page = idx / elemsPerBox`, `inPageOffset = (idx % elemsPerBox) * elemSize`.
	// Bypass the standard IndexExpression(BoxValueExpression(...), idx) path
	// since that translates to a single box_extract on a non-existent box.
	if (auto const* ident = dynamic_cast<Identifier const*>(&m_indexAccess.baseExpression()))
	{
		auto const* varDecl = dynamic_cast<VariableDeclaration const*>(
			ident->annotation().referencedDeclaration);
		if (varDecl && varDecl->isStateVariable() && !varDecl->isConstant() && !varDecl->immutable()
			&& !m_indexAccess.annotation().willBeWrittenTo)
		{
			// Read context only — write context is owned by SolAssignment's
			// tryHandleMultiBoxArrayWrite early-out (which emits box_replace
			// at the right page/offset). A ReinterpretCast cannot be a valid
			// Lvalue in puya, so we never return one here.
			auto* baseWtype = m_ctx.typeMapper.map(varDecl->type());
			if (builder::StorageMapper::isMultiBoxArray(baseWtype))
			{
				auto idxExpr = m_indexAccess.indexExpression()
					? buildExpr(*m_indexAccess.indexExpression()) : nullptr;
				if (idxExpr)
					return buildMultiBoxAccess(varDecl->name(), baseWtype, std::move(idxExpr));
			}
		}
	}

	auto base = buildExpr(m_indexAccess.baseExpression());
	std::shared_ptr<awst::Expression> index;
	if (m_indexAccess.indexExpression())
		index = buildExpr(*m_indexAccess.indexExpression());

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
					idxExpr = builder::TypeCoercion::implicitNumericCast(
						std::move(idxExpr), awst::WType::uint64Type(), m_loc);
				idxBuilder = m_ctx.builderForInstance(
					TypeProvider::uint256(), idxExpr);
			}
			if (idxBuilder)
			{
				auto result = baseBuilder->index(*idxBuilder, m_loc);
				if (result)
					return result->resolve();
			}
		}
	}

	// Regular array index
	if (index && index->wtype == awst::WType::biguintType())
		index = builder::TypeCoercion::implicitNumericCast(
			std::move(index), awst::WType::uint64Type(), m_loc);

	// bytes / bytesN indexing → extract3(base, index, 1) → bytes[1]
	// Solidity `bytes[i]` returns a `bytes1` value. Puya doesn't support
	// IndexExpression on raw bytes. Only applied in read contexts — the
	// assignment path needs a separate replace3-based lvalue handler which
	// we don't emit from here.
	if (base->wtype
		&& (base->wtype == awst::WType::bytesType()
			|| base->wtype->kind() == awst::WTypeKind::Bytes)
		&& !m_indexAccess.annotation().willBeWrittenTo
		&& index)
	{
		auto* bytes1Type = m_ctx.typeMapper.createType<awst::BytesWType>(1);
		auto one = awst::makeOne(m_loc);
		return awst::makeExtract3(
			std::move(base), std::move(index), std::move(one), m_loc, bytes1Type);
	}

	auto* expectedType = m_ctx.typeMapper.map(m_indexAccess.annotation().type);
	auto* actualElemType = expectedType;
	if (base->wtype && base->wtype->kind() == awst::WTypeKind::ReferenceArray)
	{
		auto const* refArr = static_cast<awst::ReferenceArray const*>(base->wtype);
		actualElemType = const_cast<awst::WType*>(refArr->elementType());
	}
	else if (base->wtype && base->wtype->kind() == awst::WTypeKind::ARC4StaticArray)
	{
		auto const* arc4Arr = static_cast<awst::ARC4StaticArray const*>(base->wtype);
		actualElemType = const_cast<awst::WType*>(arc4Arr->elementType());
	}
	else if (base->wtype && base->wtype->kind() == awst::WTypeKind::ARC4DynamicArray)
	{
		auto const* arc4Arr = static_cast<awst::ARC4DynamicArray const*>(base->wtype);
		actualElemType = const_cast<awst::WType*>(arc4Arr->elementType());
	}

	auto e = awst::makeIndexExpression(std::move(base), std::move(index), actualElemType, m_loc);

	// Decode ARC4 element to native type if needed (for rvalue usage)
	// Only decode when element is ARC4 and expected type is native (not ARC4)
	if (actualElemType->name() != expectedType->name())
	{
		bool elemIsArc4 = false;
		switch (actualElemType->kind())
		{
		case awst::WTypeKind::ARC4UIntN:
		case awst::WTypeKind::ARC4StaticArray:
		case awst::WTypeKind::ARC4DynamicArray:
			elemIsArc4 = true; break;
		default: break;
		}
		bool expectedIsNative = true;
		switch (expectedType->kind())
		{
		case awst::WTypeKind::ARC4UIntN:
		case awst::WTypeKind::ARC4StaticArray:
		case awst::WTypeKind::ARC4DynamicArray:
		case awst::WTypeKind::ARC4Struct:
		case awst::WTypeKind::ARC4Tuple:
			expectedIsNative = false; break;
		default: break;
		}
		if (elemIsArc4 && expectedIsNative)
		{
			auto decode = awst::makeARC4Decode(std::move(e), expectedType, m_loc);
			return decode;
		}
	}
	return e;
}

std::shared_ptr<awst::Expression> SolIndexAccess::buildMultiBoxAccess(
	std::string const& _varName,
	awst::WType const* _arrWtype,
	std::shared_ptr<awst::Expression> _idxExpr)
{
	using ::puyasol::builder::StorageMapper;

	unsigned elemSize = StorageMapper::arc4StaticArrayElementSize(_arrWtype);
	unsigned elemsPerBox = StorageMapper::elementsPerBox(_arrWtype);

	// Coerce index to uint64 for arithmetic.
	if (_idxExpr->wtype != awst::WType::uint64Type())
		_idxExpr = builder::TypeCoercion::implicitNumericCast(
			std::move(_idxExpr), awst::WType::uint64Type(), m_loc);

	// Pin idx to a temp local so we can reference it twice (page + offset).
	static int s_mbCounter = 0;
	std::string idxVarName = "__mb_idx_" + std::to_string(s_mbCounter++);
	auto idxVar = awst::makeVarExpression(idxVarName, awst::WType::uint64Type(), m_loc);
	m_ctx.prePendingStatements.push_back(
		awst::makeAssignmentStatement(idxVar, std::move(_idxExpr), m_loc));

	// page = idx / elemsPerBox
	auto idxRead1 = awst::makeVarExpression(idxVarName, awst::WType::uint64Type(), m_loc);
	auto epbConst1 = awst::makeIntegerConstant(elemsPerBox, m_loc);
	auto pageExpr = awst::makeUInt64BinOp(
		std::move(idxRead1), awst::UInt64BinaryOperator::FloorDiv,
		std::move(epbConst1), m_loc);

	// inPageOffset = (idx % elemsPerBox) * elemSize
	auto idxRead2 = awst::makeVarExpression(idxVarName, awst::WType::uint64Type(), m_loc);
	auto epbConst2 = awst::makeIntegerConstant(elemsPerBox, m_loc);
	auto remExpr = awst::makeUInt64BinOp(
		std::move(idxRead2), awst::UInt64BinaryOperator::Mod,
		std::move(epbConst2), m_loc);
	auto elemSizeConst = awst::makeIntegerConstant(elemSize, m_loc);
	auto offsetExpr = awst::makeUInt64BinOp(
		std::move(remExpr), awst::UInt64BinaryOperator::Mult,
		std::move(elemSizeConst), m_loc);

	// boxKey = bytes(varName) ++ itob(page)
	auto nameBytes = awst::makeUtf8BytesConstant(_varName, m_loc, awst::WType::boxKeyType());
	auto pageItob = awst::makeItob(std::move(pageExpr), m_loc);
	auto boxKey = awst::makeConcat(std::move(nameBytes), std::move(pageItob), m_loc);
	boxKey->wtype = awst::WType::boxKeyType();

	auto const* sa = static_cast<awst::ARC4StaticArray const*>(_arrWtype);
	auto const* elemArc4Type = sa->elementType();

	if (m_indexAccess.annotation().willBeWrittenTo)
	{
		// LHS context: caller will write to this. We can't return a writable
		// "box[offset]" expression in AWST; instead the assignment handler
		// detects multi-box arrays and emits box_replace directly. Fall back
		// to a marker expression that SolAssignment recognises — but that
		// requires plumbing through SolAssignment.cpp too. As a first cut,
		// just emit the read path; writes via this path will fail until
		// SolAssignment.cpp is extended. (Tracked as follow-up.)
		auto extract = awst::makeIntrinsicCall("box_extract", awst::WType::bytesType(), m_loc);
		extract->stackArgs.push_back(std::move(boxKey));
		extract->stackArgs.push_back(std::move(offsetExpr));
		extract->stackArgs.push_back(awst::makeIntegerConstant(elemSize, m_loc));
		auto cast = awst::makeReinterpretCast(std::move(extract),
			const_cast<awst::WType*>(elemArc4Type), m_loc);
		return cast;
	}

	// Read path: box_extract(boxKey, inPageOffset, elemSize) returning the
	// raw element bytes, reinterpreted as the element's ARC4 type, then
	// optionally ARC4Decode'd to the native expected type.
	auto extract = awst::makeIntrinsicCall("box_extract", awst::WType::bytesType(), m_loc);
	extract->stackArgs.push_back(std::move(boxKey));
	extract->stackArgs.push_back(std::move(offsetExpr));
	extract->stackArgs.push_back(awst::makeIntegerConstant(elemSize, m_loc));

	auto* expectedType = m_ctx.typeMapper.map(m_indexAccess.annotation().type);
	auto* elemArc4 = const_cast<awst::WType*>(elemArc4Type);
	auto cast = awst::makeReinterpretCast(std::move(extract), elemArc4, m_loc);

	if (expectedType && expectedType != elemArc4
		&& expectedType->name() != elemArc4->name())
	{
		bool elemIsArc4 = false;
		switch (elemArc4->kind())
		{
		case awst::WTypeKind::ARC4UIntN:
		case awst::WTypeKind::ARC4StaticArray:
		case awst::WTypeKind::ARC4DynamicArray:
		case awst::WTypeKind::ARC4Struct:
			elemIsArc4 = true; break;
		default: break;
		}
		bool expectedIsNative = expectedType->kind() != awst::WTypeKind::ARC4UIntN
			&& expectedType->kind() != awst::WTypeKind::ARC4StaticArray
			&& expectedType->kind() != awst::WTypeKind::ARC4DynamicArray
			&& expectedType->kind() != awst::WTypeKind::ARC4Struct;
		if (elemIsArc4 && expectedIsNative)
		{
			auto decode = awst::makeARC4Decode(std::move(cast), expectedType, m_loc);
			return decode;
		}
	}
	return cast;
}

std::shared_ptr<awst::Expression> SolIndexAccess::handleSlicedIndex()
{
	// Fold `root[a:b][c:d]...[i]` into `root[effective_offset + i]` with
	// bounds checks, preserving Solidity semantics where each slice level
	// reverts on start > end, end > parent_length, and index >= slice_length.
	//
	// The root base must be an ArrayType whose element type maps to ARC4
	// (uint256[] calldata, etc.). Slice-of-slice of-slice chains are
	// flattened bottom-up: the cumulative offset is the sum of all starts
	// and the cumulative length is (outermost_end - outermost_start) after
	// per-level clamping by the enclosing slice.

	using namespace solidity::frontend;

	// Walk down the IndexRangeAccess chain to find the root base.
	// Also peel off type-conversion FunctionCall wrappers like
	// `uint256[](x[s:e])` — Solidity inserts these when the slice is assigned
	// to a local with an explicit array type.
	auto peelCast = [](Expression const& e) -> Expression const& {
		Expression const* cur = &e;
		while (auto const* call = dynamic_cast<FunctionCall const*>(cur))
		{
			if (call->annotation().kind.set()
				&& *call->annotation().kind == FunctionCallKind::TypeConversion
				&& !call->arguments().empty())
				cur = call->arguments()[0].get();
			else
				break;
		}
		return *cur;
	};
	std::vector<IndexRangeAccess const*> slices;
	Expression const* cur = &peelCast(m_indexAccess.baseExpression());
	while (auto const* r = dynamic_cast<IndexRangeAccess const*>(cur))
	{
		slices.push_back(r);
		cur = &peelCast(r->baseExpression());
	}
	// slices is outermost→innermost from AST walk; reverse to innermost→outermost
	// so we apply slices in source order (closest-to-root first).
	std::reverse(slices.begin(), slices.end());

	auto const* rootArrType = dynamic_cast<ArrayType const*>(cur->annotation().type);
	if (!rootArrType || rootArrType->isByteArrayOrString())
		return nullptr; // fall through to default handling

	auto rootBase = buildExpr(*cur);

	// Stash the root base in a temp so we can reference it both for ArrayLength
	// and for indexing without duplicating the (possibly expensive) evaluation.
	std::string idSuffix = std::to_string(m_indexAccess.id());
	std::string rootVarName = "__slice_root_" + idSuffix;
	auto rootVar = awst::makeVarExpression(rootVarName, rootBase->wtype, m_loc);
	m_ctx.prePendingStatements.push_back(
		awst::makeAssignmentStatement(rootVar, rootBase, m_loc));

	auto makeLen = [&](std::shared_ptr<awst::Expression> arr) -> std::shared_ptr<awst::Expression> {
		return awst::makeArrayLength(std::move(arr), awst::WType::uint64Type(), m_loc);
	};

	// Initial cumulative offset = 0, length = len(root)
	std::shared_ptr<awst::Expression> cumOffset
		= awst::makeZero(m_loc);
	std::shared_ptr<awst::Expression> cumLength = makeLen(
		awst::makeVarExpression(rootVarName, rootBase->wtype, m_loc));

	// Stash length in a temp so we can use it in the end-default and the
	// bounds check without re-emitting ArrayLength.
	std::string lenVarName = "__slice_rootlen_" + idSuffix;
	auto lenVar = awst::makeVarExpression(lenVarName, awst::WType::uint64Type(), m_loc);
	m_ctx.prePendingStatements.push_back(
		awst::makeAssignmentStatement(lenVar, cumLength, m_loc));
	cumLength = awst::makeVarExpression(lenVarName, awst::WType::uint64Type(), m_loc);

	int sliceIx = 0;
	for (auto const* rg: slices)
	{
		std::string sIx = idSuffix + "_" + std::to_string(sliceIx++);
		std::string startName = "__slice_s_" + sIx;
		std::string endName = "__slice_e_" + sIx;

		std::shared_ptr<awst::Expression> startExpr;
		if (rg->startExpression())
			startExpr = buildExpr(*rg->startExpression());
		else
			startExpr = awst::makeZero(m_loc);
		startExpr = builder::TypeCoercion::implicitNumericCast(
			std::move(startExpr), awst::WType::uint64Type(), m_loc);

		std::shared_ptr<awst::Expression> endExpr;
		if (rg->endExpression())
			endExpr = buildExpr(*rg->endExpression());
		else
			endExpr = cumLength;
		endExpr = builder::TypeCoercion::implicitNumericCast(
			std::move(endExpr), awst::WType::uint64Type(), m_loc);

		// Stash start/end in temps.
		auto startVar = awst::makeVarExpression(startName, awst::WType::uint64Type(), m_loc);
		m_ctx.prePendingStatements.push_back(
			awst::makeAssignmentStatement(startVar, startExpr, m_loc));
		auto endVar = awst::makeVarExpression(endName, awst::WType::uint64Type(), m_loc);
		m_ctx.prePendingStatements.push_back(
			awst::makeAssignmentStatement(endVar, endExpr, m_loc));

		// assert(start <= end)
		{
			auto cmp = awst::makeNumericCompare(
				awst::makeVarExpression(startName, awst::WType::uint64Type(), m_loc),
				awst::NumericComparison::Lte,
				awst::makeVarExpression(endName, awst::WType::uint64Type(), m_loc),
				m_loc);
			m_ctx.prePendingStatements.push_back(awst::makeExpressionStatement(
				awst::makeAssert(std::move(cmp), m_loc, "slice: start > end"), m_loc));
		}

		// assert(end <= parent_length) — cumLength is the length before this slice
		{
			auto cmp = awst::makeNumericCompare(
				awst::makeVarExpression(endName, awst::WType::uint64Type(), m_loc),
				awst::NumericComparison::Lte,
				cumLength,
				m_loc);
			m_ctx.prePendingStatements.push_back(awst::makeExpressionStatement(
				awst::makeAssert(std::move(cmp), m_loc, "slice: end > length"), m_loc));
		}

		// cumOffset += start
		cumOffset = awst::makeUInt64BinOp(
			std::move(cumOffset), awst::UInt64BinaryOperator::Add,
			awst::makeVarExpression(startName, awst::WType::uint64Type(), m_loc),
			m_loc);

		// cumLength = end - start
		cumLength = awst::makeUInt64BinOp(
			awst::makeVarExpression(endName, awst::WType::uint64Type(), m_loc),
			awst::UInt64BinaryOperator::Sub,
			awst::makeVarExpression(startName, awst::WType::uint64Type(), m_loc),
			m_loc);

		// Stash updated cumLength in a per-level var so next iteration's
		// bounds check / end-default can reference it symbolically.
		std::string nextLenName = "__slice_l_" + sIx;
		auto nextLenVar = awst::makeVarExpression(nextLenName, awst::WType::uint64Type(), m_loc);
		m_ctx.prePendingStatements.push_back(
			awst::makeAssignmentStatement(nextLenVar, cumLength, m_loc));
		cumLength = awst::makeVarExpression(nextLenName, awst::WType::uint64Type(), m_loc);

		std::string nextOffName = "__slice_o_" + sIx;
		auto nextOffVar = awst::makeVarExpression(nextOffName, awst::WType::uint64Type(), m_loc);
		m_ctx.prePendingStatements.push_back(
			awst::makeAssignmentStatement(nextOffVar, cumOffset, m_loc));
		cumOffset = awst::makeVarExpression(nextOffName, awst::WType::uint64Type(), m_loc);
	}

	// Now the index access: bounds-check i < cumLength, then access root[cumOffset + i].
	auto idx = buildExpr(*m_indexAccess.indexExpression());
	idx = builder::TypeCoercion::implicitNumericCast(
		std::move(idx), awst::WType::uint64Type(), m_loc);

	std::string idxName = "__slice_i_" + idSuffix;
	auto idxVar = awst::makeVarExpression(idxName, awst::WType::uint64Type(), m_loc);
	m_ctx.prePendingStatements.push_back(
		awst::makeAssignmentStatement(idxVar, idx, m_loc));

	// assert(index < slice_length)
	{
		auto cmp = awst::makeNumericCompare(
			awst::makeVarExpression(idxName, awst::WType::uint64Type(), m_loc),
			awst::NumericComparison::Lt,
			cumLength,
			m_loc);
		m_ctx.prePendingStatements.push_back(awst::makeExpressionStatement(
			awst::makeAssert(std::move(cmp), m_loc, "slice index out of bounds"), m_loc));
	}

	// effective = offset + i
	auto effective = awst::makeUInt64BinOp(
		std::move(cumOffset), awst::UInt64BinaryOperator::Add,
		awst::makeVarExpression(idxName, awst::WType::uint64Type(), m_loc),
		m_loc);

	// Determine element type on the root array
	auto* rawElemType = m_ctx.typeMapper.map(rootArrType->baseType());
	auto* arc4ElemType = m_ctx.typeMapper.mapSolTypeToARC4(rootArrType->baseType());

	auto indexExpr = awst::makeIndexExpression(awst::makeVarExpression(rootVarName, rootBase->wtype, m_loc), std::move(effective), arc4ElemType, m_loc);

	bool needsDecode = rawElemType != arc4ElemType && rawElemType->name() != arc4ElemType->name();
	if (needsDecode)
	{
		auto decode = awst::makeARC4Decode(std::move(indexExpr), rawElemType, m_loc);
		return decode;
	}
	return indexExpr;
}


} // namespace puyasol::builder::sol_ast
