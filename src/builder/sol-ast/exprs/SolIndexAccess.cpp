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

	auto boxExpr = std::make_shared<awst::BoxValueExpression>();
	boxExpr->sourceLocation = m_loc;
	boxExpr->wtype = arrWType;
	boxExpr->key = boxKey;
	boxExpr->existsAssertionMessage = std::nullopt;

	std::shared_ptr<awst::Expression> baseExprForRead = boxExpr;
	if (!m_indexAccess.annotation().willBeWrittenTo)
	{
		auto defaultVal = builder::TypeCoercion::makeDefaultValue(arrWType, m_loc);
		auto sg = std::make_shared<awst::StateGet>();
		sg->sourceLocation = m_loc;
		sg->wtype = arrWType;
		sg->field = boxExpr;
		sg->defaultValue = defaultVal;
		baseExprForRead = sg;
	}

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
		auto extract = awst::makeIntrinsicCall("extract3", m_ctx.typeMapper.createType<awst::BytesWType>(1), m_loc);
		extract->stackArgs.push_back(baseExprForRead);
		extract->stackArgs.push_back(std::move(idx));
		auto one = awst::makeIntegerConstant("1", m_loc);
		extract->stackArgs.push_back(std::move(one));
		return extract;
	}

	auto indexExpr = std::make_shared<awst::IndexExpression>();
	indexExpr->sourceLocation = m_loc;
	indexExpr->wtype = elemType;
	indexExpr->base = m_indexAccess.annotation().willBeWrittenTo ? boxExpr : baseExprForRead;
	indexExpr->index = std::move(idx);

	if (m_indexAccess.annotation().willBeWrittenTo)
		return indexExpr;

	// Only ARC4Decode if element needs decoding to native type
	bool needsDecode = rawElemType != elemType && rawElemType->name() != elemType->name();
	if (needsDecode)
	{
		auto decode = std::make_shared<awst::ARC4Decode>();
		decode->sourceLocation = m_loc;
		decode->wtype = rawElemType;
		decode->value = std::move(indexExpr);
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
	Type const* rootMappingType = nullptr;
	if (auto const* ident = dynamic_cast<Identifier const*>(cursor))
	{
		varName = ident->name();
		rootMappingType = ident->annotation().type;

		// Storage pointer alias: `mapping storage m = m1; m[k] = v;`
		// The identifier resolves to a local with a registered alias;
		// the box prefix must match the underlying state variable, not
		// the local's name, or writes land under the wrong key.
		if (auto const* decl = ident->annotation().referencedDeclaration)
		{
			auto aliasIt = m_ctx.storageAliases.find(decl->id());
			if (aliasIt != m_ctx.storageAliases.end())
			{
				auto const* expr = aliasIt->second.get();
				// Unwrap StateGet → underlying state expression
				if (auto const* sg = dynamic_cast<awst::StateGet const*>(expr))
					expr = sg->field.get();
				// Peel off FieldExpressions so we pick the *root* state var
				// name; key synthesis works for root-level mapping aliases,
				// not nested field mappings.
				while (auto const* fe = dynamic_cast<awst::FieldExpression const*>(expr))
					expr = fe->base.get();
				std::shared_ptr<awst::Expression> keyExpr;
				if (auto const* appState = dynamic_cast<awst::AppStateExpression const*>(expr))
					keyExpr = appState->key;
				else if (auto const* boxVal = dynamic_cast<awst::BoxValueExpression const*>(expr))
					keyExpr = boxVal->key;
				if (auto const* bc = dynamic_cast<awst::BytesConstant const*>(keyExpr.get()))
					varName = std::string(bc->value.begin(), bc->value.end());
			}
		}
	}
	else if (auto const* ma = dynamic_cast<MemberAccess const*>(cursor))
	{
		varName = ma->memberName();
		rootMappingType = ma->annotation().type;
	}

	std::reverse(indexExprs.begin(), indexExprs.end());

	std::vector<awst::WType const*> declaredKeyWTypes;
	{
		Type const* walkType = rootMappingType;
		for (size_t i = 0; i < indexExprs.size(); ++i)
		{
			if (auto const* mt = dynamic_cast<MappingType const*>(walkType))
			{
				declaredKeyWTypes.push_back(m_ctx.typeMapper.map(mt->keyType()));
				walkType = mt->valueType();
			}
			else
			{
				declaredKeyWTypes.push_back(nullptr);
				if (auto const* at = dynamic_cast<ArrayType const*>(walkType))
					walkType = at->baseType();
				else
					break;
			}
		}
	}

	awst::WType const* valueWType = nullptr;
	if (auto const* mappingType = dynamic_cast<MappingType const*>(baseType))
	{
		Type const* vt = mappingType->valueType();
		while (auto const* nested = dynamic_cast<MappingType const*>(vt))
			vt = nested->valueType();
		valueWType = m_ctx.typeMapper.map(vt);
	}
	else
		valueWType = m_ctx.typeMapper.map(m_indexAccess.annotation().type);

	auto e = std::make_shared<awst::BoxValueExpression>();
	e->sourceLocation = m_loc;
	e->wtype = valueWType;

	// Build the box key prefix. For mapping-storage-ref parameters, the
	// prefix is a runtime bytes value (the caller passes the state variable
	// name). For regular state variables, it's a compile-time constant.
	std::shared_ptr<awst::Expression> prefix;
	std::string mappingKeyParam;
	if (auto const* ident = dynamic_cast<Identifier const*>(cursor))
		if (auto const* decl = ident->annotation().referencedDeclaration)
			mappingKeyParam = m_ctx.mappingKeyParams.count(decl->id())
				? m_ctx.mappingKeyParams.at(decl->id()) : "";
	if (!mappingKeyParam.empty())
	{
		// Dynamic prefix from function parameter (bytes value at runtime)
		auto var = awst::makeVarExpression(mappingKeyParam, awst::WType::bytesType(), m_loc);
		prefix = std::move(var);
	}
	else
	{
		prefix = awst::makeUtf8BytesConstant(varName, m_loc, awst::WType::boxKeyType());
	}

	if (!indexExprs.empty())
	{
		std::vector<std::shared_ptr<awst::Expression>> keyParts;
		for (size_t ki = 0; ki < indexExprs.size(); ++ki)
		{
			auto translated = buildExpr(*indexExprs[ki]);
			awst::WType const* keyWType = (ki < declaredKeyWTypes.size() && declaredKeyWTypes[ki])
				? declaredKeyWTypes[ki] : translated->wtype;

			if (keyWType != translated->wtype)
				translated = builder::TypeCoercion::implicitNumericCast(
					std::move(translated), keyWType, m_loc);

			std::shared_ptr<awst::Expression> keyBytes;
			if (keyWType == awst::WType::uint64Type())
			{
				auto itob = awst::makeIntrinsicCall("itob", awst::WType::bytesType(), m_loc);
				itob->stackArgs.push_back(std::move(translated));
				keyBytes = std::move(itob);
			}
			else if (keyWType == awst::WType::biguintType())
			{
				auto reinterpret = awst::makeReinterpretCast(std::move(translated), awst::WType::bytesType(), m_loc);

				auto padWidth = awst::makeIntegerConstant("32", m_loc);

				auto pad = awst::makeIntrinsicCall("bzero", awst::WType::bytesType(), m_loc);
				pad->stackArgs.push_back(std::move(padWidth));

				auto cat = awst::makeIntrinsicCall("concat", awst::WType::bytesType(), m_loc);
				cat->stackArgs.push_back(std::move(pad));
				cat->stackArgs.push_back(std::move(reinterpret));

				auto lenCall = awst::makeIntrinsicCall("len", awst::WType::uint64Type(), m_loc);
				lenCall->stackArgs.push_back(cat);

				auto width2 = awst::makeIntegerConstant("32", m_loc);

				auto offset = awst::makeIntrinsicCall("-", awst::WType::uint64Type(), m_loc);
				offset->stackArgs.push_back(std::move(lenCall));
				offset->stackArgs.push_back(std::move(width2));

				auto width3 = awst::makeIntegerConstant("32", m_loc);

				auto extract = awst::makeIntrinsicCall("extract3", awst::WType::bytesType(), m_loc);
				extract->stackArgs.push_back(std::move(cat));
				extract->stackArgs.push_back(std::move(offset));
				extract->stackArgs.push_back(std::move(width3));
				keyBytes = std::move(extract);
			}
			else
			{
				auto reinterpret = awst::makeReinterpretCast(std::move(translated), awst::WType::bytesType(), m_loc);
				keyBytes = std::move(reinterpret);
			}
			keyParts.push_back(std::move(keyBytes));
		}

		std::shared_ptr<awst::Expression> compositeKey;
		if (keyParts.size() == 1)
			compositeKey = std::move(keyParts[0]);
		else
		{
			compositeKey = std::move(keyParts[0]);
			for (size_t i = 1; i < keyParts.size(); ++i)
			{
				auto concat = awst::makeIntrinsicCall("concat", awst::WType::bytesType(), m_loc);
				concat->stackArgs.push_back(std::move(compositeKey));
				concat->stackArgs.push_back(std::move(keyParts[i]));
				compositeKey = std::move(concat);
			}
		}

		auto hashCall = awst::makeIntrinsicCall("sha256", awst::WType::bytesType(), m_loc);
		hashCall->stackArgs.push_back(std::move(compositeKey));
		compositeKey = std::move(hashCall);

		auto boxKey = std::make_shared<awst::BoxPrefixedKeyExpression>();
		boxKey->sourceLocation = m_loc;
		boxKey->wtype = awst::WType::boxKeyType();
		boxKey->prefix = prefix;
		boxKey->key = std::move(compositeKey);
		e->key = std::move(boxKey);
	}
	else
		e->key = std::move(prefix);

	if (m_indexAccess.annotation().willBeWrittenTo)
		return e;

	auto defaultVal = builder::StorageMapper::makeDefaultValue(e->wtype, m_loc);
	auto stateGet = std::make_shared<awst::StateGet>();
	stateGet->sourceLocation = m_loc;
	stateGet->wtype = e->wtype;
	stateGet->field = e;
	stateGet->defaultValue = defaultVal;
	return stateGet;
}

std::shared_ptr<awst::Expression> SolIndexAccess::handleRegularIndex()
{
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
		auto extract = std::make_shared<awst::IntrinsicCall>();
		extract->sourceLocation = m_loc;
		extract->opCode = "extract3";
		auto* bytes1Type = m_ctx.typeMapper.createType<awst::BytesWType>(1);
		extract->wtype = bytes1Type;
		extract->stackArgs.push_back(std::move(base));
		extract->stackArgs.push_back(std::move(index));
		auto one = awst::makeIntegerConstant("1", m_loc);
		extract->stackArgs.push_back(std::move(one));
		return extract;
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

	auto e = std::make_shared<awst::IndexExpression>();
	e->sourceLocation = m_loc;
	e->base = std::move(base);
	e->index = std::move(index);
	e->wtype = actualElemType;

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
			auto decode = std::make_shared<awst::ARC4Decode>();
			decode->sourceLocation = m_loc;
			decode->wtype = expectedType;
			decode->value = std::move(e);
			return decode;
		}
	}
	return e;
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
	std::vector<IndexRangeAccess const*> slices;
	Expression const* cur = &m_indexAccess.baseExpression();
	while (auto const* r = dynamic_cast<IndexRangeAccess const*>(cur))
	{
		slices.push_back(r);
		cur = &r->baseExpression();
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

	auto makeLen = [&](std::shared_ptr<awst::Expression> arr) {
		auto lenNode = std::make_shared<awst::ArrayLength>();
		lenNode->sourceLocation = m_loc;
		lenNode->wtype = awst::WType::uint64Type();
		lenNode->array = std::move(arr);
		return std::static_pointer_cast<awst::Expression>(lenNode);
	};

	// Initial cumulative offset = 0, length = len(root)
	std::shared_ptr<awst::Expression> cumOffset
		= awst::makeIntegerConstant("0", m_loc);
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
			startExpr = awst::makeIntegerConstant("0", m_loc);
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

	auto indexExpr = std::make_shared<awst::IndexExpression>();
	indexExpr->sourceLocation = m_loc;
	indexExpr->base = awst::makeVarExpression(rootVarName, rootBase->wtype, m_loc);
	indexExpr->index = std::move(effective);
	indexExpr->wtype = arc4ElemType;

	bool needsDecode = rawElemType != arc4ElemType && rawElemType->name() != arc4ElemType->name();
	if (needsDecode)
	{
		auto decode = std::make_shared<awst::ARC4Decode>();
		decode->sourceLocation = m_loc;
		decode->wtype = rawElemType;
		decode->value = std::move(indexExpr);
		return decode;
	}
	return indexExpr;
}

std::shared_ptr<awst::Expression> SolIndexAccess::toAwst()
{
	auto const* baseType = m_indexAccess.baseExpression().annotation().type;

	// Calldata/memory slice indexing: `root[a:b]...[i]`. We fold the slice
	// chain into a direct index on the root array; the bytes-substring3 path
	// would drop the element type and produce bytes[1] instead of the
	// declared element (uint256 etc).
	if (dynamic_cast<solidity::frontend::IndexRangeAccess const*>(
			&m_indexAccess.baseExpression()))
	{
		if (auto result = handleSlicedIndex())
			return result;
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

	auto slice = awst::makeIntrinsicCall("substring3", m_ctx.typeMapper.map(m_rangeAccess.annotation().type), m_loc);
	slice->stackArgs.push_back(std::move(base));
	slice->stackArgs.push_back(std::move(start));
	slice->stackArgs.push_back(std::move(end));
	return slice;
}

} // namespace puyasol::builder::sol_ast
