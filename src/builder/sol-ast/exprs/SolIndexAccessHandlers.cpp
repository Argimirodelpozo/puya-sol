/// @file SolIndexAccessHandlers.cpp — per-shape index-access handlers.
/// toAwst dispatchers remain in SolIndexAccess.cpp.

#include "builder/sol-ast/exprs/SolIndexAccess.h"
#include "builder/sol-ast/MappingPrefix.h"
#include "awst/NameGen.h"
#include "builder/ProgramAnalysis.h"
#include "builder/sol-ast/members/SolLengthAccess.h"
#include "builder/sol-eb/NodeBuilder.h"
#include "builder/storage/StorageMapper.h"
#include "builder/sol-types/Arc4Defaults.h"
#include "builder/sol-types/TypeMapper.h"
#include "builder/sol-types/TypeCoercion.h"
#include "awst/WType.h"

#include <functional>
#include <limits>

#include <libsolidity/ast/AST.h>
#include <libsolidity/ast/TypeProvider.h>

namespace puyasol::builder::sol_ast
{
using namespace solidity::frontend;
}
namespace puyasol::builder::sol_ast
{

std::shared_ptr<awst::Expression> SolIndexAccess::signExtendSignedElement(
	std::shared_ptr<awst::Expression> _decoded)
{
	// Delegate to shared TypeCoercion helper so this and sol-eb array builder agree.
	return builder::TypeCoercion::signExtendSignedElement(
		std::move(_decoded), m_indexAccess.annotation().type, m_loc);
}

std::shared_ptr<awst::Expression> SolIndexAccess::handleDynamicArrayAccess()
{
	auto const* arrType = dynamic_cast<ArrayType const*>(
		m_indexAccess.baseExpression().annotation().type);
	auto* rawElemType = m_ctx.typeMapper.map(arrType->baseType());
	auto* elemType = m_ctx.typeMapper.mapSolTypeToARC4(arrType->baseType());
	auto* arrWType = m_ctx.typeMapper.map(arrType);

	// Box key: a box-backed array state var is keyed by its name; a box-keyed array REF
	// param (handle model) is keyed by the runtime bytes the caller passed, so a[i] reads
	// the CALLER's box. (Field WRITES go through tryHandleBoxedArrayElemWrite's box_replace.)
	std::shared_ptr<awst::BoxValueExpression> boxExpr;
	if (auto const* ident = dynamic_cast<Identifier const*>(&m_indexAccess.baseExpression()))
	{
		std::string keyParam;
		auto const* decl = ident->annotation().referencedDeclaration;
		if (decl)
			keyParam = m_scope.findMappingKeyParam(decl->id());
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
				boxName = m_ctx.storageMapper.physicalBindingFor(*stateVar).name;
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
	// stateDynArrayLength so it can never disagree with `.length`/push/pop.
	// Covers reads AND the write lvalue (both built here). The length helper
	// accepts either a physical state key or a storage-ref parameter's runtime
	// key, so both representations get the same recursive-shape bounds rule.
	if (arrType->isDynamicallySized() && !arrType->isByteArrayOrString()
		&& !builder::computeEncodedElementSize(elemType).fixedBytes())
		if (auto const* ident = dynamic_cast<Identifier const*>(&m_indexAccess.baseExpression()))
			if (auto const* decl = dynamic_cast<VariableDeclaration const*>(
					ident->annotation().referencedDeclaration); decl)
			{
				std::shared_ptr<awst::Expression> length;
				auto const& keyParam = m_scope.findMappingKeyParam(decl->id());
				if (!keyParam.empty())
					length = SolLengthAccess::stateDynArrayLengthForKey(
						m_ctx,
						awst::makeReinterpretCast(
							awst::makeVarExpression(
								keyParam, awst::WType::bytesType(), m_loc),
							awst::WType::boxKeyType(), m_loc),
						arrType, m_loc);
				else if (decl->isStateVariable() && !decl->isConstant()
					&& !decl->immutable())
					length = SolLengthAccess::stateDynArrayLength(
						m_ctx,
						m_ctx.storageMapper.physicalBindingFor(*decl).name,
						arrType, m_loc);
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

	bool needsDecode = !awst::structurallyEquivalent(rawElemType, elemType);
	if (needsDecode)
		return signExtendSignedElement(
			awst::makeARC4Decode(std::move(indexExpr), rawElemType, m_loc));
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
	// `(m = m2)[k]`: emit the assignment (side effect: update storageAliases),
	// then resolve from the RHS. Also peel parenthesised TupleExpression wrappers.
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

	// ARRAY levels in the chain (mapping(K=>V)[] a → a[i][k]) fold the element
	// index into the derived box key. Collect the type at every level so fixed
	// bounds use solc's declared length and dynamic bounds read the length from
	// that level's current runtime prefix. This works identically for state
	// roots, mapping values, aliases, and box-keyed storage-ref parameters.
	std::vector<ArrayType const*> arrayLevels(indexExprs.size(), nullptr);
	{
		Type const* w = ctx.rootMappingType;
		for (size_t i = 0; i < indexExprs.size() && w; ++i)
		{
			if (auto const* mt = dynamic_cast<MappingType const*>(w))
				w = mt->valueType();
			else if (auto const* at = dynamic_cast<ArrayType const*>(w))
			{
				arrayLevels[i] = at;
				w = at->baseType();
			}
			else
				break;
		}
	}
	auto e = std::make_shared<awst::BoxValueExpression>();
	e->sourceLocation = m_loc;
	e->wtype = resolveValueWType(baseType);

	auto prefix = buildInitialPrefix(cursor, varName, ctx.aliasOverridePrefix);

	if (!indexExprs.empty())
	{
		// Per-layer hash derivation (Solidity-style): start with the initial
		// prefix and apply `sha256(keyBytes ++ currentPrefix)` per layer.
		std::shared_ptr<awst::Expression> currentPrefix = std::move(prefix);
		// A function-returned/otherwise computed storage prefix participates in
		// both bounds checks and key derivation. Evaluate it once before walking
		// the recursive container type.
		if (currentPrefix
			&& !dynamic_cast<awst::VarExpression const*>(currentPrefix.get())
			&& !dynamic_cast<awst::BytesConstant const*>(currentPrefix.get()))
		{
			std::string name = "__sol_prefix_" + std::to_string(
				awst::NameGen::next("SolIndexAccessHandlers.prefixTempCounter"));
			auto const* prefixWType = currentPrefix->wtype;
			m_ctx.preEffects().push_back(awst::makeAssignmentStatement(
				awst::makeVarExpression(name, prefixWType, m_loc),
				std::move(currentPrefix), m_loc));
			currentPrefix = awst::makeVarExpression(
				name, prefixWType, m_loc);
		}

		// Keep the actual serialized array value alongside the logical key.
		// Nested arrays are encoded inside their parent box; `prefix ++ index`
		// is an identity for descendant mapping boxes, not a standalone box that
		// holds the nested array length. Walking the value tree makes bounds
		// checks rank-independent without inventing boxes for inner arrays.
		Type const* walkContainer = ctx.rootMappingType;
		std::shared_ptr<awst::Expression> currentArrayValue;
		auto boxedArrayValue = [&](std::shared_ptr<awst::Expression> key,
			ArrayType const* at) -> std::shared_ptr<awst::Expression> {
			auto const* wt = m_ctx.typeMapper.map(at);
			auto box = awst::makeBoxValueExpression(std::move(key), wt, m_loc);
			return builder::StorageMapper::makeStateGetWithDefault(
				std::move(box), wt, m_loc);
		};
		// Only a genuine state-var box holds its array at its own key. An alias
		// or storage-ref prefix names a DESCENDANT mapping box, so seeding the
		// value walk from it reads a box nothing ever created.
		if (auto const* rootArray = dynamic_cast<ArrayType const*>(walkContainer);
			rootArray && ctx.rootIsStateVarBox)
			currentArrayValue = boxedArrayValue(currentPrefix, rootArray);

		for (size_t ki = 0; ki < indexExprs.size(); ++ki)
		{
			auto translated = buildExpr(*indexExprs[ki]);
			awst::WType const* keyWType = (ki < declaredKeyWTypes.size() && declaredKeyWTypes[ki])
				? declaredKeyWTypes[ki] : awst::WType::uint64Type();

			// Materialise a SIDE-EFFECTING key to a temp BEFORE any coercion. The
			// derived box key is referenced twice in a compound `m[k()] += x` /
			// `delete m[k()]` (read current + write), so a side-effecting key (`k()`
			// with cnt++) must evaluate ONCE. The guard is a bare AssignmentExpression,
			// but a SIGNED sub-word key's implicitNumericCast (sign-extend) would WRAP
			// that AssignmentExpression and hide it from the check — so the coercion
			// must run AFTER the materialisation, not before. Unsigned keys matched the
			// key type (no cast) and were already materialised; signed keys ran k()
			// twice. Found by the corpus-mutation fuzzer (mapping_key_side_effect_once
			// uint256->int48).
			// A SIDE-EFFECTING key (`m[k()]` where k() bumps a counter) is embedded in
			// the derived box key, which a compound `+= ` / `delete` references twice
			// (read current + write). An AssignmentExpression key was materialised; a
			// call-valued key (SubroutineCallExpression) was NOT — the unsigned case
			// only survived because puya CSE-merged the two IDENTICAL derivations, but a
			// SIGNED key's sign-extension makes them differ, defeating CSE, so k() ran
			// twice. Materialise call-valued keys too, once, before coercion.
			if (dynamic_cast<awst::AssignmentExpression const*>(translated.get())
				|| dynamic_cast<awst::SubroutineCallExpression const*>(translated.get()))
			{
				std::string tempName = "__sol_idx_" + std::to_string(awst::NameGen::next("SolIndexAccessHandlers.idxTempCounter"));
				auto tempVar = awst::makeVarExpression(tempName, translated->wtype, m_loc);
				auto saveStmt = awst::makeAssignmentStatement(
					tempVar, std::move(translated), m_loc);
				m_ctx.preEffects().push_back(std::move(saveStmt));
				translated = tempVar;
			}

			// ARRAY level: assert idx < length BEFORE the key-width coercion so a
			// wide index is compared un-truncated. The idx is referenced by both
			// the assert and the key layer — materialise to a temp unless it is
			// already re-creatable (var / integer constant).
			if (auto const* at = arrayLevels[ki])
			{
				std::shared_ptr<awst::Expression> bound;
				if (!at->isDynamicallySized())
				{
					if (at->length() <= std::numeric_limits<uint64_t>::max())
						bound = awst::makeIntegerConstant(at->length().str(), m_loc);
				}
				else if (currentArrayValue)
					bound = awst::makeArrayLength(
						currentArrayValue, awst::WType::uint64Type(), m_loc);
				else if (ctx.rootIsStateVarBox)
					bound = SolLengthAccess::stateDynArrayLengthForKey(
						m_ctx, currentPrefix, at, m_loc);
				// else: no length is addressable from this prefix. Reading one
				// anyway box_extracts a box that was never created — for
				// `mapping(uint=>uint)[] storage b = a[i]; b[b.length-1][k] = v`
				// it looked for a box named after the LOCAL, while that array's
				// length lives inside box `a`, and the whole call died on "no
				// such box 0x62". Skipping the assert only loses an EVM Panic
				// 0x32 on a shape that never had one.
				if (bound)
				{
					if (!dynamic_cast<awst::VarExpression const*>(translated.get())
						&& !dynamic_cast<awst::IntegerConstant const*>(translated.get()))
					{
						std::string tempName = "__sol_idx_" + std::to_string(
							awst::NameGen::next("SolIndexAccessHandlers.idxTempCounter"));
						auto tempVar = awst::makeVarExpression(
							tempName, translated->wtype, m_loc);
						m_ctx.preEffects().push_back(
							awst::makeAssignmentStatement(
								tempVar, std::move(translated), m_loc));
						translated = tempVar;
					}
					std::shared_ptr<awst::Expression> idxRef;
					if (auto const* ve =
							dynamic_cast<awst::VarExpression const*>(translated.get()))
						idxRef = awst::makeVarExpression(ve->name, ve->wtype, m_loc);
					else if (auto const* ic =
							dynamic_cast<awst::IntegerConstant const*>(translated.get()))
						idxRef = awst::makeIntegerConstant(ic->value, m_loc, ic->wtype);
					if (idxRef)
					{
						if (bound->wtype != idxRef->wtype)
							bound = builder::TypeCoercion::implicitNumericCast(
								std::move(bound), idxRef->wtype, m_loc);
						auto cmp = awst::makeNumericCompare(std::move(idxRef),
							awst::NumericComparison::Lt, std::move(bound), m_loc);
						m_ctx.preEffects().push_back(
							awst::makeExpressionStatement(
								awst::makeAssert(std::move(cmp), m_loc,
									"array index out of bounds"),
								m_loc));
					}
				}
			}

			if (keyWType != translated->wtype)
				translated = builder::TypeCoercion::implicitNumericCast(
					std::move(translated), keyWType, m_loc);

			Type const* nextContainer = nullptr;
			bool const arrayStep = dynamic_cast<ArrayType const*>(walkContainer);
			if (auto const* at = dynamic_cast<ArrayType const*>(walkContainer))
				nextContainer = at->baseType();
			else if (auto const* mt = dynamic_cast<MappingType const*>(walkContainer))
				nextContainer = mt->valueType();

			if (arrayStep && currentArrayValue
				&& dynamic_cast<ArrayType const*>(nextContainer))
			{
				auto const* childW = m_ctx.typeMapper.mapSolTypeToARC4(nextContainer);
				currentArrayValue = awst::makeIndexExpression(
					currentArrayValue, translated, childW, m_loc);
			}
			else
				currentArrayValue = nullptr;

			currentPrefix = awst::makeMappingKeyLayer(
				translated, keyWType, currentPrefix, m_loc);

			// A mapping value starts a new serialized box. If that value is an
			// array, resume value-directed traversal at the just-derived key;
			// consecutive array levels remain inline and use IndexExpression above.
			if (!arrayStep)
				if (auto const* nextArray = dynamic_cast<ArrayType const*>(nextContainer))
					currentArrayValue = boxedArrayValue(currentPrefix, nextArray);
			walkContainer = nextContainer;
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

		// Storage alias `mapping storage m = m1`: box prefix must track the
		// underlying state var, not the local name, or writes land wrong.
		if (auto const* decl = ident->annotation().referencedDeclaration)
		{
			// State vars key by their PHYSICAL binding name (declaration
			// identity): colliding inherited names get disambiguated, and the
			// public getter seeds its hash chain from the same binding.
			if (auto const* stateVar = dynamic_cast<VariableDeclaration const*>(decl);
				stateVar && stateVar->isStateVariable()
				&& !stateVar->isConstant() && !stateVar->immutable())
			{
				out.varName = m_ctx.storageMapper.physicalBindingFor(*stateVar).name;
				out.rootIsStateVarBox = true;
			}
			auto const* alias = m_scope.findStorageAlias(decl->id());
			if (alias)
			{
				// An alias re-roots the chain at somebody else's box. The
				// shared resolver COLLECTS the field names the alias walked —
				// the old inline peel dropped them, so `Inner storage p =
				// st.a; p.m[k]` keyed utf8(st)++"m" (losing "a") and read a
				// different box than st.a.m[k].
				out.rootIsStateVarBox = false;
				if (auto holder = resolveHolderRoot(m_ctx, m_scope, *ident, m_loc))
					out.aliasOverridePrefix = awst::makeReinterpretCast(
						std::move(holder), awst::WType::boxKeyType(), m_loc);
			}
		}
	}
	else if (auto const* ma = dynamic_cast<MemberAccess const*>(_cursor))
	{
		out.varName = ma->memberName();
		out.rootMappingType = ma->annotation().type;

		// One rule covers every "mapping reached through struct fields" shape:
		// walk the MemberAccess chain base.f1...fn down to its ROOT, resolve the
		// root's holder prefix, then append each field name. Without the holder
		// prefix the derivation started at bare utf8(fn), which collapsed every
		// container element / same-typed holder onto ONE box — silent
		// cross-element data corruption (night-3 campaign). Roots:
		//   - registered storage-ref param (V4 `self.inner[k]`): the runtime
		//     bytes value the caller passed (selfPrefix convention);
		//   - storage ALIAS (`S storage p = mm[a];` / base-ctor `A(m[1])`):
		//     the aliased holder's box/appstate key;
		//   - plain struct STATE VAR: utf8(name);
		//   - mapping element `mm[a]`: its derived box key — exactly what
		//     SolInternalCall::extractMappingKeyPrefix lifts, so direct and
		//     ref-param access key the same boxes;
		//   - storage-array element `arr[i]`: utf8(name) ++ itob(i), with an
		//     EVM-style bounds assert (Panic 0x32) on i.
		// Unmatched roots (function calls, memory bases) keep the legacy
		// bare-field prefix.
		std::vector<std::string> fields{ma->memberName()};
		Expression const* rootE = &ma->expression();
		while (auto const* innerMa = dynamic_cast<MemberAccess const*>(rootE))
		{
			fields.push_back(innerMa->memberName());
			rootE = &innerMa->expression();
		}
		std::reverse(fields.begin(), fields.end());

		std::shared_ptr<awst::Expression> holder;
		if (auto const* baseIdx = dynamic_cast<IndexAccess const*>(rootE))
		{
			// Peel every array index to the declaration root. The holder identity
			// is structural: root key ++ itob(i0) ++ ... ++ itob(iN). The previous
			// direct-base match handled arr[i].m but collapsed arr[i][j].m (and all
			// deeper forms) onto the bare field key.
			std::vector<IndexAccess const*> arrayPath;
			Expression const* arrayRoot = baseIdx;
			while (auto const* index = dynamic_cast<IndexAccess const*>(arrayRoot))
			{
				if (!index->indexExpression())
					break;
				arrayPath.push_back(index);
				arrayRoot = &index->baseExpression();
			}
			std::reverse(arrayPath.begin(), arrayPath.end());
			auto const* arrId = dynamic_cast<Identifier const*>(arrayRoot);
			auto const* arrDecl = arrId ? dynamic_cast<VariableDeclaration const*>(
				arrId->annotation().referencedDeclaration) : nullptr;
			if (arrDecl && arrDecl->isStateVariable() && !arrayPath.empty()
				&& dynamic_cast<ArrayType const*>(arrDecl->type()))
			{
				auto const binding =
					m_ctx.storageMapper.physicalBindingFor(*arrDecl).name;
				holder = awst::makeUtf8BytesConstant(binding, m_loc);
				Type const* walk = arrDecl->type();
				auto const* rootW = m_ctx.typeMapper.map(walk);
				auto rootBox = builder::StorageMapper::makeTopLevelBoxExpr(
					binding, rootW, m_loc);
				std::shared_ptr<awst::Expression> arrayValue =
					builder::StorageMapper::makeStateGetWithDefault(
						std::move(rootBox), rootW, m_loc);
				for (size_t depth = 0; depth < arrayPath.size(); ++depth)
				{
					auto const* arrType = dynamic_cast<ArrayType const*>(walk);
					if (!arrType)
					{
						holder = nullptr;
						break;
					}
					auto idx = builder::TypeCoercion::checkedIndexToUint64(
						m_ctx.preEffects(),
						buildExpr(*arrayPath[depth]->indexExpression()), m_loc);
					std::string tmpName = "__sol_arridx_" + std::to_string(
						awst::NameGen::next("SolIndexAccessHandlers.arrIdxCounter"));
					auto tmpVar = [&]() {
						return awst::makeVarExpression(
							tmpName, awst::WType::uint64Type(), m_loc);
					};
					m_ctx.preEffects().push_back(awst::makeAssignmentStatement(
						tmpVar(), std::move(idx), m_loc));

					// Fixed bounds come from solc. A dynamic level's length lives at
					// the current holder prefix, before this level appends its index.
					std::shared_ptr<awst::Expression> bound;
					if (!arrType->isDynamicallySized()
						&& arrType->length() <= std::numeric_limits<uint64_t>::max())
						bound = awst::makeIntegerConstant(arrType->length().str(), m_loc);
					else if (arrayValue)
						bound = awst::makeArrayLength(
							arrayValue, awst::WType::uint64Type(), m_loc);
					if (bound)
					{
						auto cmp = awst::makeNumericCompare(
							tmpVar(), awst::NumericComparison::Lt,
							std::move(bound), m_loc);
						m_ctx.preEffects().push_back(awst::makeExpressionStatement(
							awst::makeAssert(std::move(cmp), m_loc,
								"array index out of bounds"), m_loc));
					}
					holder = awst::makeConcat(
						std::move(holder), awst::makeItob(tmpVar(), m_loc), m_loc);
					walk = arrType->baseType();
					if (dynamic_cast<ArrayType const*>(walk) && arrayValue)
						arrayValue = awst::makeIndexExpression(
							arrayValue, tmpVar(),
							m_ctx.typeMapper.mapSolTypeToARC4(walk), m_loc);
					else
						arrayValue = nullptr;
				}
			}
			else if (dynamic_cast<MappingType const*>(
				baseIdx->baseExpression().annotation().type))
			{
				auto built = awst::unwrapStateGet(buildExpr(*rootE));
				if (auto const* box =
						dynamic_cast<awst::BoxValueExpression const*>(built.get()))
					holder = box->key;
			}
		}
		else
			// Identifier roots (mapping-key param / storage alias incl. the
			// field names the alias walked / state var) — the SHARED
			// derivation, so direct access and ref-param arguments cannot
			// key different boxes (MappingPrefix.h).
			holder = resolveHolderRoot(m_ctx, m_scope, *rootE, m_loc);

		if (holder)
		{
			for (auto const& f: fields)
				holder = awst::makeConcat(std::move(holder),
					awst::makeUtf8BytesConstant(f, m_loc), m_loc);
			out.aliasOverridePrefix = awst::makeReinterpretCast(
				std::move(holder), awst::WType::boxKeyType(), m_loc);
		}
	}
	// `f()[k]`: call returns bytes = holder name = runtime key prefix.
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
	// Multi-box array (>32KB): split across `<name>` ++ `itob(page)` boxes.
	// Standard IndexExpression would box_extract a non-existent single box.
	if (auto const* ident = dynamic_cast<Identifier const*>(&m_indexAccess.baseExpression()))
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
					return buildMultiBoxAccess(
						m_ctx.storageMapper.physicalBindingFor(*varDecl).name,
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

	// Regular array index
	if (index && index->wtype == awst::WType::biguintType())
	{
		// biguint→uint64 cast duplicates its operand (slices concat(bzero(8),idx)
		// and takes its length), so side-effecting idx like `a[--i]` or
		// `a[f()]` runs twice. Pin to temp first (T2: call-valued escaped).
		if (dynamic_cast<awst::AssignmentExpression const*>(index.get())
			|| dynamic_cast<awst::SubroutineCallExpression const*>(index.get()))
		{
			std::string tempName = "__sol_ixc_" + std::to_string(
				awst::NameGen::next("SolIndexAccess.coercedIndex"));
			auto tempVar = awst::makeVarExpression(tempName, index->wtype, m_loc);
			m_ctx.preEffects().push_back(
				awst::makeAssignmentStatement(tempVar, std::move(index), m_loc));
			index = tempVar;
		}
		index = builder::TypeCoercion::checkedIndexToUint64(
			m_ctx.preEffects(), std::move(index), m_loc);
	}

	// bytes/bytesN index: puya rejects IndexExpression on bytes; use extract3.
	// Write context unsupported (needs replace3-based handler).
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
	if (!awst::structurallyEquivalent(actualElemType, expectedType))
	{
		bool const elemIsArc4 = builder::isArc4EncodedType(actualElemType);
		bool const expectedIsNative = !builder::isArc4EncodedType(expectedType);
		if (elemIsArc4 && expectedIsNative)
		{
			std::shared_ptr<awst::Expression> decode =
				awst::makeARC4Decode(std::move(e), expectedType, m_loc);
			// Sign-extend only for reads; write targets need bare decode (valid lvalue).
			if (!m_indexAccess.annotation().willBeWrittenTo)
				decode = signExtendSignedElement(std::move(decode));
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
	_idxExpr = builder::TypeCoercion::checkedIndexToUint64(
		m_ctx.preEffects(), std::move(_idxExpr), m_loc);

	// Pin idx to a temp local so we can reference it twice (page + offset).
	std::string idxVarName = "__mb_idx_" + std::to_string(awst::NameGen::next("SolIndexAccessHandlers.s_mbCounter"));
	auto idxVar = awst::makeVarExpression(idxVarName, awst::WType::uint64Type(), m_loc);
	m_ctx.preEffects().push_back(
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
		// Write context: AWST has no writable box[offset] lvalue; SolAssignment
		// handles multi-box writes via box_replace. Emit read path as fallback.
		auto extract = awst::makeBoxExtract(
			std::move(boxKey), std::move(offsetExpr),
			awst::makeIntegerConstant(elemSize, m_loc), m_loc);
		return awst::makeReinterpretCast(std::move(extract),
			const_cast<awst::WType*>(elemArc4Type), m_loc);
	}

	// Read: box_extract → reinterpret as ARC4 type → optionally ARC4Decode.
	auto extract = awst::makeBoxExtract(
		std::move(boxKey), std::move(offsetExpr),
		awst::makeIntegerConstant(elemSize, m_loc), m_loc);

	auto* expectedType = m_ctx.typeMapper.map(m_indexAccess.annotation().type);
	auto* elemArc4 = const_cast<awst::WType*>(elemArc4Type);
	auto cast = awst::makeReinterpretCast(std::move(extract), elemArc4, m_loc);

	if (expectedType && !awst::structurallyEquivalent(expectedType, elemArc4))
	{
		bool const elemIsArc4 = builder::isArc4EncodedType(elemArc4);
		bool const expectedIsNative = !builder::isArc4EncodedType(expectedType);
		if (elemIsArc4 && expectedIsNative)
			return signExtendSignedElement(
				awst::makeARC4Decode(std::move(cast), expectedType, m_loc));
	}
	return cast;
}

std::shared_ptr<awst::Expression> SolIndexAccess::handleSlicedIndex()
{
	// Fold `root[a:b][c:d]...[i]` into `root[cumOffset + i]`.
	// Each slice level reverts on start>end / end>parent_length / i>=slice_length.
	// Chains flatten bottom-up: cumOffset = sum of starts, cumLength = end-start.

	using namespace solidity::frontend;

	// Walk IndexRangeAccess chain to root, peeling type-conversion wrappers
	// like `uint256[](x[s:e])` that Solidity inserts for typed slice locals.
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
	// Reverse so we process innermost (closest-to-root) slice first.
	std::reverse(slices.begin(), slices.end());

	auto const* rootArrType = dynamic_cast<ArrayType const*>(cur->annotation().type);
	if (!rootArrType || rootArrType->isByteArrayOrString())
		return nullptr; // fall through to default handling

	auto rootBase = buildExpr(*cur);

	// Stash root in temp to avoid duplicating possibly-expensive evaluation.
	std::string idSuffix = std::to_string(m_indexAccess.id());
	std::string rootVarName = "__slice_root_" + idSuffix;
	auto rootVar = awst::makeVarExpression(rootVarName, rootBase->wtype, m_loc);
	m_ctx.preEffects().push_back(
		awst::makeAssignmentStatement(rootVar, rootBase, m_loc));

	auto makeLen = [&](std::shared_ptr<awst::Expression> arr) -> std::shared_ptr<awst::Expression> {
		return awst::makeArrayLength(std::move(arr), awst::WType::uint64Type(), m_loc);
	};

	// Initial cumulative offset = 0, length = len(root)
	std::shared_ptr<awst::Expression> cumOffset
		= awst::makeZero(m_loc);
	std::shared_ptr<awst::Expression> cumLength = makeLen(
		awst::makeVarExpression(rootVarName, rootBase->wtype, m_loc));

	// Stash length in temp for end-default and bounds check.
	std::string lenVarName = "__slice_rootlen_" + idSuffix;
	auto lenVar = awst::makeVarExpression(lenVarName, awst::WType::uint64Type(), m_loc);
	m_ctx.preEffects().push_back(
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
		startExpr = builder::TypeCoercion::checkedIndexToUint64(
			m_ctx.preEffects(), std::move(startExpr), m_loc);

		std::shared_ptr<awst::Expression> endExpr;
		if (rg->endExpression())
			endExpr = buildExpr(*rg->endExpression());
		else
			endExpr = cumLength;
		endExpr = builder::TypeCoercion::checkedIndexToUint64(
			m_ctx.preEffects(), std::move(endExpr), m_loc);

		// Stash start/end in temps.
		auto startVar = awst::makeVarExpression(startName, awst::WType::uint64Type(), m_loc);
		m_ctx.preEffects().push_back(
			awst::makeAssignmentStatement(startVar, startExpr, m_loc));
		auto endVar = awst::makeVarExpression(endName, awst::WType::uint64Type(), m_loc);
		m_ctx.preEffects().push_back(
			awst::makeAssignmentStatement(endVar, endExpr, m_loc));

		// assert(start <= end)
		{
			auto cmp = awst::makeNumericCompare(
				awst::makeVarExpression(startName, awst::WType::uint64Type(), m_loc),
				awst::NumericComparison::Lte,
				awst::makeVarExpression(endName, awst::WType::uint64Type(), m_loc),
				m_loc);
			m_ctx.preEffects().push_back(awst::makeExpressionStatement(
				awst::makeAssert(std::move(cmp), m_loc, "slice: start > end"), m_loc));
		}

		// assert(end <= parent_length) — cumLength is the length before this slice
		{
			auto cmp = awst::makeNumericCompare(
				awst::makeVarExpression(endName, awst::WType::uint64Type(), m_loc),
				awst::NumericComparison::Lte,
				cumLength,
				m_loc);
			m_ctx.preEffects().push_back(awst::makeExpressionStatement(
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
		m_ctx.preEffects().push_back(
			awst::makeAssignmentStatement(nextLenVar, cumLength, m_loc));
		cumLength = awst::makeVarExpression(nextLenName, awst::WType::uint64Type(), m_loc);

		std::string nextOffName = "__slice_o_" + sIx;
		auto nextOffVar = awst::makeVarExpression(nextOffName, awst::WType::uint64Type(), m_loc);
		m_ctx.preEffects().push_back(
			awst::makeAssignmentStatement(nextOffVar, cumOffset, m_loc));
		cumOffset = awst::makeVarExpression(nextOffName, awst::WType::uint64Type(), m_loc);
	}

	// Now the index access: bounds-check i < cumLength, then access root[cumOffset + i].
	auto idx = buildExpr(*m_indexAccess.indexExpression());
	idx = builder::TypeCoercion::checkedIndexToUint64(
		m_ctx.preEffects(), std::move(idx), m_loc);

	std::string idxName = "__slice_i_" + idSuffix;
	auto idxVar = awst::makeVarExpression(idxName, awst::WType::uint64Type(), m_loc);
	m_ctx.preEffects().push_back(
		awst::makeAssignmentStatement(idxVar, idx, m_loc));

	// assert(index < slice_length)
	{
		auto cmp = awst::makeNumericCompare(
			awst::makeVarExpression(idxName, awst::WType::uint64Type(), m_loc),
			awst::NumericComparison::Lt,
			cumLength,
			m_loc);
		m_ctx.preEffects().push_back(awst::makeExpressionStatement(
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

	bool needsDecode = !awst::structurallyEquivalent(rawElemType, arc4ElemType);
	if (needsDecode)
	{
		auto decode = awst::makeARC4Decode(std::move(indexExpr), rawElemType, m_loc);
		return decode;
	}
	return indexExpr;
}


} // namespace puyasol::builder::sol_ast
