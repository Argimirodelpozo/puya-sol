/// @file SolInternalCall.cpp
/// Internal function call resolution and SubroutineCallExpression building.

#include "builder/sol-ast/calls/SolInternalCall.h"
#include "builder/sol-types/RefParamPassing.h"
#include "builder/sol-ast/exprs/SolIndexAccess.h"
#include "builder/ProgramAnalysis.h"
#include "builder/SolcFacts.h"
#include "builder/sol-ast/EvmSlotLowering.h"
#include "builder/sol-ast/MappingPrefix.h"
#include "builder/storage/EvmLayoutMode.h"
#include "awst/NameGen.h"
#include "builder/sol-types/SolIntType.h"
#include "builder/BuildArtifacts.h"
#include "builder/ReturnWirePlan.h"
#include "builder/sol-ast/EffectScan.h"
#include "builder/sol-ast/StorageRefPointer.h"
#include "builder/contract/EvmMemoryCodec.h"
#include "builder/itxn/AsaIntrinsics.h"
#include "builder/abi/Arc4Stdlib.h"
#include "builder/itxn/CallResolver.h"
#include "builder/itxn/FunctionPointerBuilder.h"
#include "builder/sol-types/FunctionPointerKind.h"
#include "builder/sol-types/TypeMapper.h"
#include "builder/sol-types/Arc4Defaults.h"
#include "builder/sol-types/TypeCoercion.h"
#include "builder/sol-types/ConversionPlan.h"
#include "builder/sol-eb/AssignmentHelper.h"
#include "builder/storage/StorageMapper.h"
#include "builder/storage/StoragePlace.hpp"
#include "Logger.h"

#include <algorithm>
#include <functional>
#include <map>
#include <set>
#include <vector>

#include <libsolidity/ast/AST.h>
#include <libsolidity/ast/ASTVisitor.h>

namespace puyasol::builder::sol_ast
{

using namespace solidity::frontend;

namespace {
// Mirrors puya's `_is_referable_expression`: peel index/field/tuple-item
// layers to the root variable name, or "" for non-referable lvalues.
std::string referableVarName(awst::Expression const* e)
{
	while (e)
	{
		if (auto const* v = dynamic_cast<awst::VarExpression const*>(e))
			return v->name;
		if (auto const* ix = dynamic_cast<awst::IndexExpression const*>(e))
		{ e = ix->base.get(); continue; }
		if (auto const* ti = dynamic_cast<awst::TupleItemExpression const*>(e))
		{ e = ti->base.get(); continue; }
		if (auto const* fa = dynamic_cast<awst::FieldExpression const*>(e))
		{ e = fa->base.get(); continue; }
		break;
	}
	return "";
}

} // namespace

awst::WType const* SolInternalCall::returnTypeFrom(FunctionDefinition const* _funcDef)
{
	if (!_funcDef)
		return m_ctx.typeMapper.map(m_call.annotation().type);
	return m_ctx.typeMapper.functionReturnPlan(*_funcDef).internalType;
}

namespace
{

/// Any-rank array element path (`a[i][j]...`) rooted in an Identifier whose declaration is an array — the shape whose root box key …
struct BoxedArrayPath
{
	Identifier const* root = nullptr;
	VariableDeclaration const* declaration = nullptr;
	std::vector<IndexAccess const*> indices;
};

std::optional<BoxedArrayPath> boxedArrayPath(Expression const& expression)
{
	BoxedArrayPath result;
	auto const* cursor = &expression;
	while (auto const* index = dynamic_cast<IndexAccess const*>(cursor))
	{
		if (!index->indexExpression())
			return std::nullopt;
		result.indices.push_back(index);
		cursor = &index->baseExpression();
	}
	result.root = dynamic_cast<Identifier const*>(cursor);
	result.declaration = result.root
		? dynamic_cast<VariableDeclaration const*>(
			result.root->annotation().referencedDeclaration) : nullptr;
	if (!result.declaration || result.indices.empty()
		|| !dynamic_cast<ArrayType const*>(result.declaration->type()))
		return std::nullopt;
	std::reverse(result.indices.begin(), result.indices.end());
	return result;
}

std::shared_ptr<awst::Expression> boxedArrayKey(
	eb::ContractContext& ctx, Context& scope,
	BoxedArrayPath const& path, awst::SourceLocation const& loc)
{
	auto const& runtimeKey = scope.bindings.mappingKeyParams.get(path.declaration->id());
	if (!runtimeKey.empty())
		return awst::makeVarExpression(
		runtimeKey, awst::WType::bytesType(), loc);
	if (path.declaration->isStateVariable())
	{
		auto binding = ctx.storageMapper.physicalBindingFor(*path.declaration);
		if (binding.kind == awst::AppStorageKind::Box)
			return awst::makeUtf8BytesConstant(
				binding.key, loc, awst::WType::bytesType());
	}
	return nullptr;
}

/// Aliasing guard: same variable in >1 arg position → puya rejects ("mutable values cannot be passed more than once", e.g.
void applyAliasingGuard(
	awst::SubroutineCallExpression& call,
	FunctionDefinition const* _funcDef,
	ParameterMutationSummary const* mutations,
	awst::SourceLocation const& m_loc)
{
	// Map arg position → param index (using-for receiver → param 0) → mutated?
	auto paramMutatedForArg = [&](size_t argIdx) -> bool {
		size_t pIdx = argIdx; // using-for receiver already occupies arg 0 == param 0
		if (pIdx >= _funcDef->parameters().size())
			return false;
		return mutations && mutations->mutates(pIdx);
	};

	// Group arg positions by aliased variable; note if any hits a mutated param.
	std::map<std::string, std::vector<size_t>> positionsByVar;
	std::set<std::string> varTouchesMutatedParam;
	for (size_t ai = 0; ai < call.args.size(); ++ai)
	{
		auto& ca = call.args[ai];
		if (!ca.value || !ca.value->wtype || ca.value->wtype->immutable())
			continue;
		std::string vn = referableVarName(ca.value.get());
		if (vn.empty())
			continue;
		positionsByVar[vn].push_back(ai);
		if (paramMutatedForArg(ai))
			varTouchesMutatedParam.insert(vn);
	}

	// Copy occurrences after the first for vars aliased across >1 position
	// and not touching a mutated param.
	for (auto const& [vn, positions] : positionsByVar)
	{
		if (positions.size() < 2 || varTouchesMutatedParam.count(vn))
			continue;
		for (size_t k = 1; k < positions.size(); ++k)
		{
			auto& ca = call.args[positions[k]];
			auto copy = std::make_shared<awst::Copy>();
			copy->sourceLocation = m_loc;
			copy->wtype = ca.value->wtype;
			copy->value = std::move(ca.value);
			ca.value = std::move(copy);
		}
	}
}



/// Per-storage-arg root tracing.
struct StorageRoot {
	size_t paramIdx = 0;
	std::shared_ptr<awst::BoxValueExpression> rootBox;
	std::shared_ptr<awst::AppStateExpression> rootAppState;
	std::vector<std::string> fieldPath;
	awst::WType const* rootType = nullptr;
	awst::WType const* storageArgType = nullptr;
};

std::vector<StorageRoot> traceStorageRoots(
	awst::SubroutineCallExpression const& call,
	std::vector<size_t> const& storageParamIndices)
{
	std::vector<StorageRoot> roots;
	roots.reserve(storageParamIndices.size());

	for (size_t pi: storageParamIndices)
	{
		StorageRoot sr;
		sr.paramIdx = pi;
		sr.storageArgType = call.args[pi].value->wtype;

		std::function<void(awst::Expression const*)> traceToRoot;
		traceToRoot = [&](awst::Expression const* e) {
			if (auto const* field = dynamic_cast<awst::FieldExpression const*>(e)) {
				sr.fieldPath.push_back(field->name);
				traceToRoot(field->base.get());
			} else if (auto const* sg = dynamic_cast<awst::StateGet const*>(e)) {
				traceToRoot(sg->field.get());
			} else if (auto const* box = dynamic_cast<awst::BoxValueExpression const*>(e)) {
				sr.rootBox = std::make_shared<awst::BoxValueExpression>(*box);
			} else if (auto const* app = dynamic_cast<awst::AppStateExpression const*>(e)) {
				sr.rootAppState = awst::makeAppStateExpression(app->key, app->wtype, app->sourceLocation);
			}
		};
		traceToRoot(call.args[pi].value.get());
		sr.rootType = sr.rootBox ? sr.rootBox->wtype
			: sr.rootAppState ? sr.rootAppState->wtype : nullptr;
		roots.push_back(std::move(sr));
	}
	return roots;
}

/// Rebuild the complete ARC4 struct path copy-on-write for a `box.field...` storage arg.
std::shared_ptr<awst::Expression> rebuildFieldPathWriteValue(
	eb::ContractContext& ctx,
	StorageRoot const& sr,
	std::shared_ptr<awst::Expression> modifiedArg,
	awst::SourceLocation const& m_loc)
{
	auto fieldPath = sr.fieldPath;
	std::reverse(fieldPath.begin(), fieldPath.end());

	std::shared_ptr<awst::Expression> fieldTarget = sr.rootBox
		? std::static_pointer_cast<awst::Expression>(sr.rootBox)
		: std::static_pointer_cast<awst::Expression>(sr.rootAppState);
	bool validPath = fieldTarget != nullptr;
	for (auto const& fieldName: fieldPath)
	{
		auto const* structType = fieldTarget
			? dynamic_cast<awst::ARC4Struct const*>(fieldTarget->wtype)
			: nullptr;
		awst::WType const* fieldType = nullptr;
		if (structType)
			for (auto const& [name, type]: structType->fields())
				if (name == fieldName)
				{
					fieldType = type;
					break;
				}
		if (!structType || !fieldType)
		{
			validPath = false;
			break;
		}
		fieldTarget = awst::makeFieldExpression(
			std::move(fieldTarget), fieldName, fieldType, m_loc);
	}
	if (validPath)
	{
		auto cow = eb::AssignmentHelper::rebuildArc4StructChainCOW(
			ctx, std::move(fieldTarget), std::move(modifiedArg), m_loc);
		return std::move(cow.assignValue);
	}
	// Reached only for a param the mutation detector flagged
	// as mutated, so dropping the write-back is a guaranteed
	// silent miscompile — fail loud instead.
	Logger::instance().error(
		"callee mutates a field path of a non-struct storage-ref "
		"argument, which cannot be written back on AVM — the "
		"mutation would be silently lost.",
		m_loc);
	return nullptr;
}

/// Unpack the augmented `(r..., sp..., mp...)` return: stash the call in a temp, rebuild the original return value, write each …
std::shared_ptr<awst::Expression> emitAugmentedCallWriteBacks(
	eb::ContractContext& ctx,
	std::shared_ptr<awst::SubroutineCallExpression> const& call,
	awst::WType const* origRetType,
	std::vector<StorageRoot> const& roots,
	std::vector<size_t> const& memoryRefParamIndices,
	std::vector<std::pair<std::string, Type const*>> const& blobWriteBacks,
	CallBoundaryPlan const& plan,
	awst::SourceLocation const& m_loc)
{
	// AWSTBuilder augments return type when storage/memory-ref params exist:
	//   non-void: (r0..rK-1, sp0..spN-1, mp0..mpM-1) — original return
	//     FLATTENED (K values, not nested WTuple).
	//   void: bare type if N+M==1; tuple otherwise.
	// Always unpack (even unresolved args) — wtype mismatch otherwise.
	bool voidReturn = (origRetType == awst::WType::voidType());
	auto const* origRetTuple = voidReturn
		? nullptr
		: dynamic_cast<awst::WTuple const*>(origRetType);

	size_t origRetCount = voidReturn
		? 0
		: (origRetTuple ? origRetTuple->types().size() : 1);

	// 1 element → bare type (puya doesn't wrap single-elem returns);
	// 2+ → WTuple.
	auto const* callTupleType = call->wtype;

	std::string tempName = "__storage_wb_" + std::to_string(awst::NameGen::next("SolInternalCall.storageWriteBackCounter"));

	auto tempVar = awst::makeVarExpression(tempName, callTupleType, m_loc);

	auto assignTemp = awst::makeAssignmentStatement(
		tempVar, std::shared_ptr<awst::Expression>(call), m_loc);
	ctx.preEffects().push_back(std::move(assignTemp));

	// Single bare-type: tempVar IS the value; no TupleItemExpression.
	size_t totalAugmented = roots.size() + memoryRefParamIndices.size();
	bool isBareSingle = (
		(voidReturn && totalAugmented == 1) ||
		(!voidReturn && totalAugmented == 0)
	);
	auto pickFromTuple = [&](size_t idx, awst::WType const* ty)
		-> std::shared_ptr<awst::Expression>
	{
		if (isBareSingle)
			return tempVar;
		auto t = awst::makeTupleItem(tempVar, static_cast<int>(idx), ty, m_loc);
		return t;
	};

	std::shared_ptr<awst::Expression> origRet;
	if (voidReturn)
	{
		origRet = awst::makeVoidConstant(m_loc);
		origRet->sourceLocation = m_loc;
		origRet->wtype = awst::WType::voidType();
	}
	else if (origRetTuple)
	{
		// Multi-value return: rebuild from flattened head (elements 0..K-1).
		auto reTuple = awst::makeTupleExpression(origRetType, m_loc);
		for (size_t i = 0; i < origRetCount; ++i)
			reTuple->items.push_back(pickFromTuple(i, origRetTuple->types()[i]));
		origRet = std::move(reTuple);
	}
	else
	{
		origRet = pickFromTuple(0, origRetType);
	}

	// Write back each storage arg that resolved to a state root.
	// Unresolved args (caller locals) have no source-of-truth to update.
	size_t baseIdx = origRetCount;
	for (size_t i = 0; i < roots.size(); ++i)
	{
		auto const& sr = roots[i];
		if (!sr.rootBox && !sr.rootAppState)
			continue;

		auto modifiedArg = pickFromTuple(baseIdx + i, sr.storageArgType);

		std::shared_ptr<awst::Expression> writeValue = modifiedArg;
		if (!sr.fieldPath.empty())
			writeValue = rebuildFieldPathWriteValue(
				ctx, sr, std::move(modifiedArg), m_loc);

		if (writeValue)
		{
			std::shared_ptr<awst::Expression> writeTarget =
				sr.rootBox ? std::static_pointer_cast<awst::Expression>(sr.rootBox)
						: std::static_pointer_cast<awst::Expression>(sr.rootAppState);

			auto writeBack = awst::makeAssignmentExpression(
				std::move(writeTarget), std::move(writeValue), m_loc, sr.rootType);

			ctx.queuePostExpression(std::move(writeBack), m_loc);
		}
	}

	// Memory-ref writeback: assign the post-call tuple slot back to the caller
	// local. A modifier-chain memory root is pointer-backed, so its value-use is
	// a materialisation rather than a VarExpression; overwrite the existing
	// scratch object instead. This preserves the alias observed by the wrapped
	// body and modifier epilogues when an internal helper mutates the argument.
	size_t memBaseIdx = baseIdx + roots.size();
	for (size_t mi = 0; mi < memoryRefParamIndices.size(); ++mi)
	{
		size_t pi = memoryRefParamIndices[mi];
		auto* memArgType = call->args[pi].value->wtype;
		auto modifiedArg = pickFromTuple(memBaseIdx + mi, memArgType);

		auto const& [blobOffset, blobType] = blobWriteBacks[mi];
		if (!blobOffset.empty())
		{
			std::vector<std::shared_ptr<awst::Statement>> writes;
			if (!builder::writeEvmMemoryValueAt(
					ctx.typeMapper, blobType, std::move(modifiedArg),
					awst::makeVarExpression(blobOffset,
						awst::WType::uint64Type(), m_loc),
					m_loc, writes))
			{
				Logger::instance().error(
					"callee mutation of this pointer-backed memory value cannot "
					"be written through without changing its root pointer",
					m_loc);
				continue;
			}
			for (auto& write: writes)
				ctx.queuePostEffect(std::move(write));
			continue;
		}

		auto const* argVar = dynamic_cast<awst::VarExpression const*>(
			call->args[pi].value.get());
		// A non-VarExpression arg (a temporary like `mut(getArray())`) has
		// no caller-visible lvalue to write back to — the mutation is
		// unobservable anyway (EVM matches). Correctly dropped, no warning.
		if (!argVar || argVar->name.empty())
			continue;

		auto target = awst::makeVarExpression(argVar->name, memArgType, m_loc);
		auto writeBack = awst::makeAssignmentExpression(
			std::move(target), std::move(modifiedArg), m_loc);

		ctx.queuePostExpression(std::move(writeBack), m_loc);
	}

	return origRet;
}

} // anonymous namespace

std::shared_ptr<awst::Expression> SolInternalCall::wrapStorageRefResult(
	std::shared_ptr<awst::Expression> _result,
	FunctionDefinition const* _funcDef)
{
	// Storage-ref pointer function: the subroutine returns the uint64
	// index of the location. Reconstitute the storage reference at the
	// call site as `IndexExpression(<stateVar>, <call>)` — a real lvalue
	// node, which puya accepts where a SubroutineCallExpression would not.
	// --evm-storage-layout: the biguint slot IS the reference — no
	// IndexExpression reconstitution.
	if (m_ctx.typeMapper.profile().evmStorageLayout)
		return _result;
	auto const* indexAccess = builder::storageRefPointerReturn(_funcDef, m_ctx.typeMapper.analysis());
	if (!indexAccess)
		return _result;
	// Box-keyed mapping-of-struct storage ref: the callee already returns the
	// bytes box-key prefix (see mapReturnType / the return-body handling). Pass
	// it through unchanged — the caller binds it as a struct-storage-ref
	// (SolVariableDeclaration) — rather than reconstituting an IndexExpression,
	// which here would be the invalid `bytes[idx] -> Struct`.
	if (builder::storageRefReturnIsBytesKeyed(_funcDef, m_ctx.typeMapper.analysis()))
		return _result;
	auto base = m_ctx.buildExpr(indexAccess->baseExpression());
	auto* elemType = m_ctx.typeMapper.map(
		_funcDef->returnParameters()[0]->type());
	return awst::makeIndexExpression(
		std::move(base), std::move(_result), elemType, m_loc);
}

std::shared_ptr<awst::Expression> SolInternalCall::extractMappingKeyPrefix(
	Expression const& argExpr)
{
	if (containsMappingType(argExpr.annotation().type))
		return storageReferenceKey(m_ctx, m_scope, argExpr, m_loc);
	// Any-rank array element path rooted in one physical box keeps that
	// root key. A companion byte offset identifies the selected struct.
	if (auto path = boxedArrayPath(argExpr))
		if (auto key = boxedArrayKey(m_ctx, m_scope, *path, m_loc))
			return key;

	// Array element (`arr[i]`) passed as a struct ref (handle-model dual handle): the element
	// is a SLICE of the array's box, not its own box — lift the ARRAY's box key here; the
	// companion offset arg (offsetForArg) carries header + i*elemSize. Mapping values (`m[k]`)
	// ARE their own box and are handled by the generic lift below.
	if (auto const* iaArr = dynamic_cast<IndexAccess const*>(&argExpr))
		if (auto const* at = dynamic_cast<ArrayType const*>(
				iaArr->baseExpression().annotation().type))
			if (!at->isByteArrayOrString())
			{
				auto baseBuilt = awst::unwrapStateGet(buildExpr(iaArr->baseExpression()));
				if (auto const* box =
						dynamic_cast<awst::BoxValueExpression const*>(baseBuilt.get()))
					return awst::makeReinterpretCast(
						box->key, awst::WType::bytesType(), m_loc);
			}

	// IndexAccess storage-ref: prefix must be the RUNTIME box key
	// (the derived mapping-entry hash), not a static name (all keys would alias).
	// Build the element access, lift its box key; callee reinterprets it.
	if (dynamic_cast<IndexAccess const*>(&argExpr))
	{
		auto built = awst::unwrapStateGet(buildExpr(argExpr));
		if (auto const* box = dynamic_cast<awst::BoxValueExpression const*>(built.get()))
			return awst::makeReinterpretCast(
				box->key, awst::WType::bytesType(), m_loc);
	}

	return storageReferenceKey(m_ctx, m_scope, argExpr, m_loc);
}

void SolInternalCall::buildSequencedArgs(
	std::vector<awst::CallArg>& args,
	FunctionDefinition const* _funcDef,
	bool _isUsingForCall,
	awst::SubroutineTarget const* target,
	bool followingEffects)
{
	// Args evaluate left-to-right on EVM (verified vs 0.8.20 + py-evm), with
	// each arg's write-backs landing before the NEXT arg — and before the call
	// itself executes. Capture each arg's queued effects; re-emitted in order
	// below once all args are built.
	std::vector<eb::ContractContext::OperandDeltas> argDeltas;
	std::vector<bool> argMayWrite;
	auto const* plan = _funcDef ? &m_ctx.typeMapper.callBoundaryPlan(*_funcDef, m_ctx.currentContract) : nullptr;
	auto const* functionType = dynamic_cast<FunctionType const*>(funcExpression().annotation().type);
	std::vector<awst::WType const*> paramTypes;
	if (plan)
		for (auto const& parameter: plan->parameters)
			paramTypes.push_back(parameter.type);
	else if (functionType)
		for (auto const* type: functionType->parameterTypes())
			paramTypes.push_back(m_ctx.typeMapper.map(type));
	static std::set<size_t> const noParameters;
	auto const& mappingStorageParamIndices = plan ? plan->keyParams : noParameters;
	auto const& evmSlotRefParamIndices = plan ? plan->slotParams : noParameters;
	auto const& blobOffsetParamIndices = plan ? plan->blobParams : noParameters;
	std::map<size_t, std::shared_ptr<awst::Expression>> offsets;
	auto keyArgument = [&](Expression const& expression, size_t pi) -> std::shared_ptr<awst::Expression> {
		auto const* array = dynamic_cast<ArrayType const*>(expression.annotation().type);
		bool largeFixed = isLargeFixedArrayRef(m_ctx.typeMapper, expression.annotation().type);
		// A key-only array parameter addresses the entire encoded box. An
		// interior dynamic array also needs parent offset-table/resize metadata;
		// passing just its parent's key silently writes the wrong array header.
		bool dynamicValue = array && !containsMappingType(array) && hasDynamicStorageShape(array);
		if (largeFixed || dynamicValue)
		{
			if (auto const* id = dynamic_cast<Identifier const*>(&expression))
				if (auto const* declaration = id->annotation().referencedDeclaration;
					declaration && !m_scope.bindings.mappingKeyParams.get(declaration->id()).empty())
					return extractMappingKeyPrefix(expression);
			auto built = buildExpr(expression);
			auto place = StoragePlace::fromRead(built);
			if (!place || place->kind != StoragePlaceKind::Box)
			{
				// Interior dynamic array of a box-stored aggregate (`self._checkpoints`
				// with `self` a box-keyed struct): pass the enclosing box key and
				// specialize the library/free callee on the field path.
				std::vector<std::string> path;
				std::shared_ptr<awst::Expression> cursor = awst::unwrapStateGet(built);
				while (auto const* field = dynamic_cast<awst::FieldExpression const*>(cursor.get()))
				{
					path.insert(path.begin(), field->name);
					cursor = awst::unwrapStateGet(field->base);
				}
				auto const* box = dynamic_cast<awst::BoxValueExpression const*>(cursor.get());
				auto const* scope = _funcDef ? _funcDef->annotation().contract : nullptr;
				bool const specializable = _funcDef && box && box->key && !path.empty() && !largeFixed
					&& (_funcDef->isFree() || (scope && scope->isLibrary()))
					&& (target && std::holds_alternative<awst::SubroutineID>(*target));
				if (specializable)
				{
					m_pathSpecs[pi] = {path, box->wtype};
					return awst::makeReinterpretCast(box->key, awst::WType::bytesType(), m_loc);
				}
				// Host-bound callee (it or a callee of it uses inline assembly on
				// storage, e.g. OpenZeppelin 5.x Checkpoints._unsafeAccess): the
				// EVM slot arithmetic only exists under --evm-storage-layout.
				bool const hostBound = _funcDef && box && !path.empty()
					&& !(target && std::holds_alternative<awst::SubroutineID>(*target));
				throw SizeError(std::string(largeFixed
					? "large fixed-array storage references require a whole-box root; interior slices are unsupported"
					: "dynamic-array storage references require a whole-box root; interior resize paths are unsupported")
					+ (hostBound ? " (the callee is bound to the contract by inline assembly on storage; compile with --evm-storage-layout)" : ""));
			}
			return awst::makeReinterpretCast(place->key, awst::WType::bytesType(), m_loc);
		}
		// Interior mapping-containing struct member (`map._keys` of an
		// EnumerableMap): the member has no whole box of its own on the direct
		// access path, so specialize the library/free callee on the field path
		// under the enclosing box instead of passing a key it cannot address.
		// (Also an aliased parameter of a specialized callee handed onward.)
		if (auto const* solType = expression.annotation().type;
			containsMappingType(solType) && !dynamic_cast<MappingType const*>(solType)
			&& _funcDef && (target && std::holds_alternative<awst::SubroutineID>(*target))
			&& (_funcDef->isFree()
				|| (_funcDef->annotation().contract && _funcDef->annotation().contract->isLibrary())))
		{
			auto holder = resolveStorageHolder(m_ctx, m_scope, expression, m_loc);
			auto place = holder.value ? StoragePlace::fromRead(holder.value) : std::nullopt;
			if (holder.key && !(place && place->kind == StoragePlaceKind::Box))
			{
				std::vector<std::string> path;
				std::shared_ptr<awst::Expression> cursor = awst::unwrapStateGet(holder.value);
				while (auto const* field = dynamic_cast<awst::FieldExpression const*>(cursor.get()))
				{
					path.insert(path.begin(), field->name);
					cursor = awst::unwrapStateGet(field->base);
				}
				if (auto const* box = dynamic_cast<awst::BoxValueExpression const*>(cursor.get());
					box && box->key && !path.empty())
				{
					m_pathSpecs[pi] = {path, box->wtype};
					return awst::makeReinterpretCast(box->key, awst::WType::bytesType(), m_loc);
				}
			}
		}
		auto key = extractMappingKeyPrefix(expression);
		if (plan && std::find(plan->offsetParams.begin(), plan->offsetParams.end(), pi) != plan->offsetParams.end())
		{
			auto offset = offsetForArg(&expression);
			auto name = "__call_offset_" + std::to_string(awst::NameGen::next("SolInternalCall.offset"));
			auto variable = awst::makeVarExpression(name, awst::WType::uint64Type(), m_loc);
			m_ctx.preEffects().push_back(awst::makeAssignmentStatement(variable, std::move(offset), m_loc));
			offsets.emplace(pi, std::move(variable));
		}
		return key;
	};

	// For using-for calls, prepend receiver as first arg
	if (_isUsingForCall)
	{
		auto const& funcExpr = funcExpression();
		if (auto const* memberAccess = dynamic_cast<MemberAccess const*>(&funcExpr))
		{
			awst::CallArg ca;
			auto lowered = m_ctx.lowerOperand([&]() -> std::shared_ptr<awst::Expression> {
				if (evmSlotRefParamIndices.count(0))
				{
					sol_ast::EvmSlotLowering low(m_ctx, m_scope, m_loc);
					auto addr = low.resolve(memberAccess->expression());
					return addr ? addr->slot : nullptr;
				}
				if (mappingStorageParamIndices.count(0))
					return keyArgument(memberAccess->expression(), 0);
				if (blobOffsetParamIndices.count(0))
					if (auto off = SolIndexAccess::resolveBlobOffset(
							m_ctx, m_scope, memberAccess->expression(), m_loc))
						return off;
				auto v = buildExpr(memberAccess->expression());
				if (!paramTypes.empty())
					v = builder::TypeCoercion::implicitNumericCast(
						std::move(v), paramTypes[0], m_loc);
				return v;
			}, /*_conditional=*/false);
			ca.value = std::move(lowered.value);
			argDeltas.push_back(std::move(lowered.effects));
			argMayWrite.push_back(builder::EffectScan::mayWrite(memberAccess->expression(), m_ctx));
			args.push_back(std::move(ca));
		}
	}

	// Build arguments with type coercion
	auto const sortedArgs = m_call.sortedArguments();
	for (size_t i = 0; i < sortedArgs.size(); ++i)
	{
		awst::CallArg ca;
		size_t paramIdx = _isUsingForCall ? (i + 1) : i;
		auto const* parameterType = _funcDef && paramIdx < _funcDef->parameters().size()
			? _funcDef->parameters()[paramIdx]->type()
			: functionType && paramIdx < functionType->parameterTypes().size()
				? functionType->parameterTypes()[paramIdx] : nullptr;
		auto lowered = m_ctx.lowerOperand([&]() -> std::shared_ptr<awst::Expression> {
			if (evmSlotRefParamIndices.count(paramIdx))
			{
				sol_ast::EvmSlotLowering low(m_ctx, m_scope, m_loc);
				auto addr = low.resolve(*sortedArgs[i]);
				return addr ? addr->slot : nullptr;
			}
			if (mappingStorageParamIndices.count(paramIdx))
				return keyArgument(*sortedArgs[i], paramIdx);
			// Blob param (>4KB memory aggregate): the callee takes the uint64
			// base offset (pointer model). Building the VALUE materialized the
			// whole struct and fed an ARC4Struct into the uint64 param —
			// silent garbage. Resolve the pointer instead; an unresolvable
			// shape falls through to the value build (loud type mismatch).
			if (blobOffsetParamIndices.count(paramIdx))
				if (auto off = SolIndexAccess::resolveBlobOffset(
						m_ctx, m_scope, *sortedArgs[i], m_loc))
					return off;
			auto v = buildExpr(*sortedArgs[i]);
			if (parameterType && !parameterType->dataStoredIn(DataLocation::Storage))
				v = StorageMapper::makePartialBoxReadWithDefault(
					m_ctx.typeMapper, std::move(v), m_ctx.preEffects(), m_loc);
			// Slot mode: a storage-ref arg bound to a VALUE (memory) param
			// materializes here — the slot handle can't coerce to the value
			// type (it crashed field reads: "extraction end 8 beyond length").
			if (v && paramIdx < paramTypes.size())
				v = sol_ast::EvmSlotLowering::materializeRefValue(
					m_ctx, m_scope, std::move(v),
					sortedArgs[i]->annotation().type,
					paramTypes[paramIdx], m_loc);
			if (parameterType && paramIdx < paramTypes.size())
				v = builder::ConversionPlan{
					sortedArgs[i]->annotation().type,
					parameterType,
					paramTypes[paramIdx],
					builder::ConversionPlan::Context::Argument}.emit(
						std::move(v), m_loc);
			else if (paramIdx < paramTypes.size())
				v = builder::TypeCoercion::implicitNumericCast(
					std::move(v), paramTypes[paramIdx], m_loc);
			return v;
		}, /*_conditional=*/false);
		ca.value = std::move(lowered.value);
		argDeltas.push_back(std::move(lowered.effects));
		argMayWrite.push_back(builder::EffectScan::mayWrite(*sortedArgs[i], m_ctx));
		args.push_back(std::move(ca));
	}

	// Re-emit captured arg effects in arg order. With no write-backs and no
	// direct-state-writing args this restores the pre-statements
	// byte-identically. Otherwise each arg's write-backs hoist to pre-position
	// (so later args and the callee observe them), and an earlier arg whose
	// value a LATER arg's effects could disturb is pinned first. Local reads
	// are not exempt: a later argument can assign or increment them.
	// Mutable-wtype values are never pinned —
	// a pin temp would defeat the aliasing guard below.
	{
		for (size_t ai = 0; ai < argDeltas.size(); ++ai)
		{
			bool laterEffects = followingEffects;
			for (size_t aj = ai + 1; aj < argDeltas.size(); ++aj)
				laterEffects = laterEffects
					|| !argDeltas[aj].empty() || argMayWrite[aj];
			bool pin = (laterEffects || !argDeltas[ai].post.empty())
				&& args[ai].value
				&& args[ai].value->wtype
				&& args[ai].value->wtype->immutable();
			args[ai].value = m_ctx.emitSequencedOperand(
				std::move(argDeltas[ai]), std::move(args[ai].value), pin, m_loc);
		}
	}
	if (plan)
		for (auto pi: plan->offsetParams)
			args.push_back({std::nullopt, offsets.at(pi)});
}

std::shared_ptr<awst::Expression> SolInternalCall::offsetForArg(
	Expression const* argExpr)
{
	// A storage-ref PARAM passed onward carries ITS caller-supplied
	// runtime offset — forward the offset var (bump(s) inside
	// inner(S storage s) wrote element 0 without this).
	if (argExpr)
		if (auto const* id = dynamic_cast<Identifier const*>(argExpr))
			if (auto const* vd = dynamic_cast<VariableDeclaration const*>(
					id->annotation().referencedDeclaration))
				if (auto offVar = m_scope.bindings.structRefOffsets.get(vd->id());
					!offVar.empty())
					return awst::makeVarExpression(
						offVar, awst::WType::uint64Type(), m_loc);
	if (argExpr)
		if (auto path = boxedArrayPath(*argExpr))
			if (auto key = boxedArrayKey(m_ctx, m_scope, *path, m_loc))
			{
				auto const* rootW = m_ctx.typeMapper.map(path->declaration->type());
				auto boxKey = awst::makeReinterpretCast(
					std::move(key), awst::WType::boxKeyType(), m_loc);
				auto box = awst::makeBoxValueExpression(
					std::move(boxKey), rootW, m_loc);
				std::string bytesName = "__sref_path_" + std::to_string(
					awst::NameGen::next("SolInternalCall.structRefPath"));
				m_ctx.preEffects().push_back(awst::makeAssignmentStatement(
					awst::makeVarExpression(
						bytesName, awst::WType::bytesType(), m_loc),
					awst::makeAsBytes(builder::StorageMapper::makeStateGetWithDefault(
						std::move(box), rootW, m_loc), m_loc), m_loc));
				auto bytesVar = [&]() {
					return awst::makeVarExpression(
						bytesName, awst::WType::bytesType(), m_loc);
				};
				std::shared_ptr<awst::Expression> base =
					awst::makeIntegerConstant(0, m_loc);
				Type const* current = path->declaration->type();
				for (auto const* index: path->indices)
				{
					auto const* array = dynamic_cast<ArrayType const*>(current);
					if (!array || array->isByteArrayOrString())
						return awst::makeIntegerConstant(0, m_loc);
					auto idx = builder::TypeCoercion::checkedIndexToUint64(
						m_ctx.preEffects(), buildExpr(*index->indexExpression()), m_loc);
					auto const* elemArc4 =
						m_ctx.typeMapper.mapSolTypeToARC4(array->baseType());
					uint64_t header = array->isDynamicallySized() ? 2 : 0;
					if (builder::arc4IsDynamic(elemArc4))
					{
						auto tablePos = awst::makeUInt64BinOp(
							awst::makeUInt64BinOp(base,
								awst::UInt64BinaryOperator::Add,
								awst::makeIntegerConstant(header, m_loc), m_loc),
							awst::UInt64BinaryOperator::Add,
							awst::makeUInt64BinOp(std::move(idx),
								awst::UInt64BinaryOperator::Mult,
								awst::makeIntegerConstant(2, m_loc), m_loc), m_loc);
						auto relative = awst::makeBtoi(awst::makeExtract3(
							bytesVar(), std::move(tablePos),
							awst::makeIntegerConstant(2, m_loc), m_loc), m_loc);
						base = awst::makeUInt64BinOp(
							awst::makeUInt64BinOp(base,
								awst::UInt64BinaryOperator::Add,
								awst::makeIntegerConstant(header, m_loc), m_loc),
							awst::UInt64BinaryOperator::Add,
							std::move(relative), m_loc);
					}
					else
					{
						int elemSize = builder::computeEncodedElementSize(elemArc4).fixedBytes<int>().value_or(0);
						if (elemSize <= 0)
							return awst::makeIntegerConstant(0, m_loc);
						base = awst::makeUInt64BinOp(
							awst::makeUInt64BinOp(base,
								awst::UInt64BinaryOperator::Add,
								awst::makeIntegerConstant(header, m_loc), m_loc),
							awst::UInt64BinaryOperator::Add,
							awst::makeUInt64BinOp(std::move(idx),
								awst::UInt64BinaryOperator::Mult,
								awst::makeIntegerConstant(
									static_cast<uint64_t>(elemSize), m_loc), m_loc), m_loc);
					}
					current = array->baseType();
				}
				return base;
			}
	return awst::makeIntegerConstant(0, m_loc); // whole-box → offset 0
}



std::shared_ptr<awst::Expression> SolInternalCall::buildSubroutineCall(
	awst::SubroutineTarget _target,
	awst::WType const* _returnType,
	FunctionDefinition const* _funcDef,
	bool _isUsingForCall)
{
	// External fn-ptr params use the profile-selected dual-purpose byte layout;
	// dispatch handles them.

	// The resolver supplied one exact body and target. Its signature and
	// mutation summary must stay paired; never re-resolve only the metadata.
	auto const* functionType = dynamic_cast<FunctionType const*>(m_call.expression().annotation().type);
	auto const* mutations = _funcDef && functionType
		&& functionType->kind() != FunctionType::Kind::External
		? &m_ctx.typeMapper.analysis().parameterMutations(m_ctx.currentContract, *_funcDef)
		: nullptr;
	auto const* plan = _funcDef
		? &m_ctx.typeMapper.callBoundaryPlan(*_funcDef, m_ctx.currentContract) : nullptr;
	auto const* target = std::get_if<awst::InstanceMethodTarget>(&_target);
	bool const abiEntry = _funcDef && _funcDef->isPartOfExternalInterface() && target
		&& target->memberName == eb::CallResolver::resolveMethodName(m_ctx, *_funcDef);
	auto const* emittedReturn = abiEntry
		? m_ctx.typeMapper.functionReturnPlan(*_funcDef).wireType
		: plan ? plan->augmentReturn(m_ctx.typeMapper, _returnType) : _returnType;
	auto call = awst::makeSubroutineCall(std::move(_target), emittedReturn, m_loc);

	buildSequencedArgs(call->args, _funcDef, _isUsingForCall, &call->target);
	if (!m_pathSpecs.empty())
	{
		// Retarget to the callee specialized on the interior field paths.
		auto const* symbol = std::get_if<awst::SubroutineID>(&call->target);
		if (!symbol || !_funcDef)
			throw SizeError("dynamic-array storage references require a whole-box root; interior resize paths are unsupported");
		auto& artifacts = m_ctx.typeMapper.artifacts();
		builder::BuildArtifacts::PathSpecialization spec;
		spec.function = _funcDef;
		std::string key = symbol->target;
		for (auto const& [index, pathAndType]: m_pathSpecs)
		{
			spec.params.push_back({index, pathAndType.first, pathAndType.second});
			key += "|" + std::to_string(index) + ":";
			for (auto const& member: pathAndType.first)
				key += "." + member;
			if (pathAndType.second)
				key += "@" + pathAndType.second->name();
		}
		auto found = artifacts.pathSpecializationIds.find(key);
		if (found == artifacts.pathSpecializationIds.end())
		{
			spec.id = symbol->target + "__path" + std::to_string(artifacts.pathSpecializationIds.size());
			found = artifacts.pathSpecializationIds.emplace(key, spec.id).first;
			artifacts.pendingPathSpecializations.push_back(std::move(spec));
		}
		call->target = awst::SubroutineID{found->second};
		m_pathSpecs.clear();
	}

	if (_funcDef)
		applyAliasingGuard(*call, _funcDef, mutations, m_loc);

	if (_funcDef)
	{
		auto const& plan = m_ctx.typeMapper.callBoundaryPlan(*_funcDef, m_ctx.currentContract);
		if (!plan.writeBackParams.empty())
		{
			auto roots = traceStorageRoots(*call, plan.storageWriteBackParams);
			// Solc identifies the source declaration and mutated parameter;
			// Context only supplies our representation-specific scratch pointer.
			auto const sourceArgs = m_call.sortedArguments();
			std::vector<std::pair<std::string, Type const*>> blobWriteBacks;
			for (size_t pi: plan.memoryWriteBackParams)
			{
				if (pi >= plan.parameters.size()
					|| plan.parameters[pi].passing != RefParamPassing::Value)
				{
					blobWriteBacks.emplace_back();
					continue;
				}
				Expression const* source = nullptr;
				if (_isUsingForCall && pi == 0)
					if (auto const* member = dynamic_cast<MemberAccess const*>(
							&funcExpression()))
						source = &member->expression();
				size_t const shift = _isUsingForCall ? 1 : 0;
				if (!source && pi >= shift && pi - shift < sourceArgs.size())
					source = sourceArgs[pi - shift].get();
				auto const* id = dynamic_cast<Identifier const*>(source);
				auto const* declaration = id
					? dynamic_cast<VariableDeclaration const*>(
						id->annotation().referencedDeclaration) : nullptr;
				blobWriteBacks.emplace_back(
					declaration ? m_scope.bindings.blobAggregates.get(declaration->id()) : "",
					declaration ? declaration->type() : nullptr);
			}
			auto origRet = emitAugmentedCallWriteBacks(
				m_ctx, call, _returnType, roots, plan.memoryWriteBackParams,
				blobWriteBacks, plan, m_loc);
			return wrapStorageRefResult(std::move(origRet), _funcDef);
		}
		if (abiEntry)
		{
			for (size_t pi = 0; pi < plan.parameters.size(); ++pi)
			{
				auto const& parameter = plan.parameters[pi];
				call->args[pi].value = parameter.encodeArgument(std::move(call->args[pi].value), m_loc);
				call->args[pi].name = parameter.wireName();
			}
		}
	}

	return wrapStorageRefResult(decodeCallResult(call, _returnType, m_loc), _funcDef);
}

std::shared_ptr<awst::Expression> SolInternalCall::resolveIdentifierCall(
	Identifier const& identifier)
{
	if (auto resolved = eb::CallResolver::resolveFunction(m_ctx, identifier))
		return buildSubroutineCall(std::move(resolved->target),
			returnTypeFrom(resolved->funcDef), resolved->funcDef, false);
	return buildSubroutineCall(awst::InstanceMethodTarget{identifier.name()},
		m_ctx.typeMapper.map(m_call.annotation().type), nullptr, false);
}

std::shared_ptr<awst::Expression> SolInternalCall::resolveMemberAccessCall(
	MemberAccess const& member)
{
	// Intrinsics consume source expressions and must precede argument lowering.
	if (auto result = eb::Arc4Stdlib::tryHandleCall(m_ctx, member, m_call, m_loc))
		return *result;
	if (auto result = eb::AsaIntrinsics::tryHandleCall(m_ctx, member, m_call, m_loc))
		return *result;
	if (auto resolved = eb::CallResolver::resolveFunction(m_ctx, member))
		return buildSubroutineCall(std::move(resolved->target),
			returnTypeFrom(resolved->funcDef), resolved->funcDef, resolved->isUsingForCall);

	// A self getter has a solc FunctionType but no FunctionDefinition. Its
	// emitted return uses the same element plan as PublicGetterBuilder.
	auto* native = m_ctx.typeMapper.map(m_call.annotation().type);
	if (auto const* variable = dynamic_cast<VariableDeclaration const*>(
			member.annotation().referencedDeclaration);
		variable && eb::CallResolver::plan(m_call).isSelfCall)
	{
		auto const* getter = variable->functionType(false);
		std::vector<awst::WType const*> wire;
		for (auto const* type: getter->returnParameterTypes())
			wire.push_back(planReturnElement(m_ctx.typeMapper, type,
				abiReturnNativeType(m_ctx.typeMapper, type)).wireType);
		auto* resultType = wire.size() == 1 ? wire.front()
			: m_ctx.typeMapper.createType<awst::WTuple>(std::move(wire));
		auto call = awst::makeSubroutineCall(
			awst::InstanceMethodTarget{member.memberName()}, resultType, m_loc);
		buildSequencedArgs(call->args, nullptr, false);
		for (size_t i = 0; i < call->args.size(); ++i)
		{
			auto const* type = getter->parameterTypes()[i];
			if (m_ctx.typeMapper.map(type) == awst::WType::biguintType())
			{
				auto integer = SolIntType::fromSol(type);
				if (integer && integer->isSigned && integer->bits < 256)
					call->args[i].value = TypeCoercion::maskUnsignedToWidth(
						std::move(call->args[i].value), integer->bits, m_loc);
				call->args[i].value = awst::makeARC4Encode(std::move(call->args[i].value),
					m_ctx.typeMapper.createType<awst::ARC4UIntN>(integer ? integer->bits : 256), m_loc);
			}
		}
		return decodeCallResult(std::move(call), native, m_loc);
	}
	return buildSubroutineCall(
		awst::InstanceMethodTarget{member.memberName()}, native, nullptr, false);
}

std::shared_ptr<awst::Expression> SolInternalCall::buildFunctionPointerCall(
	Expression const& callee, FunctionType const& type)
{
	// Retain the direct-call reference conventions only for immutable source
	// facts. A mutable local must read its runtime pointer, even before the
	// first syntactically encountered write or through a branch/loop.
	if (auto const* identifier = dynamic_cast<Identifier const*>(&callee))
		if (auto const* declaration = identifier->annotation().referencedDeclaration)
		{
			auto const& stable = m_ctx.typeMapper.analysis().stableFunctionPointers;
			if (auto it = stable.find(declaration->id()); it != stable.end())
			{
				auto const& initializer = SolcFacts::functionExpression(*it->second);
				auto const* member = dynamic_cast<MemberAccess const*>(&initializer);
				auto const* receiver = member ? dynamic_cast<Identifier const*>(&member->expression()) : nullptr;
				// A foreign receiver remains an actual inner application call.
				if (type.kind() == FunctionType::Kind::Internal || (receiver && receiver->name() == "this"))
					if (auto resolved = eb::CallResolver::resolveFunction(m_ctx, initializer))
						return buildSubroutineCall(std::move(resolved->target),
							returnTypeFrom(resolved->funcDef), resolved->funcDef, false);
			}
		}

	// Identifier/member/index readers own storage and ARC4 field decoding.
	// Capture effects before emitting either operand group: legacy solc
	// evaluates internal-call arguments before its callee; IR and external
	// calls evaluate the callee first.
	auto pointer = m_ctx.lower(callee, false);
	auto const* wanted = eb::FunctionPointerBuilder::mapFunctionType(m_ctx, &type);
	if (pointer.value && !awst::structurallyEquivalent(pointer.value->wtype, wanted))
		pointer.value = decodeCallResult(std::move(pointer.value), wanted, m_loc);
	bool const calleeFirst = m_ctx.viaIRSequencing || isExternalFunctionPointer(&type);
	auto emitPointer = [&]() {
		pointer.value = m_ctx.emitSequencedOperand(std::move(pointer.effects),
			std::move(pointer.value), true, m_loc);
	};
	if (calleeFirst) emitPointer();
	std::vector<awst::CallArg> arguments;
	// Even a plain pointer read can observe a change made by an argument's
	// value expression (not just its queued effects). Finish args first on legacy.
	buildSequencedArgs(arguments, nullptr, false, nullptr, !calleeFirst);
	if (!calleeFirst) emitPointer();
	std::vector<std::shared_ptr<awst::Expression>> values;
	for (auto& argument: arguments)
		values.push_back(std::move(argument.value));
	return eb::FunctionPointerBuilder::buildFunctionPointerCall(
		m_ctx, std::move(pointer.value), &type, std::move(values), m_loc);
}

std::shared_ptr<awst::Expression> SolInternalCall::toAwst()
{
	auto const plan = eb::CallResolver::plan(m_call);
	if (plan.isFunctionPointer && plan.functionType)
		return buildFunctionPointerCall(*plan.callee, *plan.functionType);
	if (auto const* identifier = dynamic_cast<Identifier const*>(plan.callee))
		return resolveIdentifierCall(*identifier);
	if (auto const* member = dynamic_cast<MemberAccess const*>(plan.callee))
		return resolveMemberAccessCall(*member);
	Logger::instance().error("could not resolve function call target", m_loc);
	return buildSubroutineCall(awst::InstanceMethodTarget{"unknown"},
		m_ctx.typeMapper.map(m_call.annotation().type), nullptr, false);
}

} // namespace puyasol::builder::sol_ast
