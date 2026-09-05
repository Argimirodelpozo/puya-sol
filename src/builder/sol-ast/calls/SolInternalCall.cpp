/// @file SolInternalCall.cpp
/// Internal function call resolution and SubroutineCallExpression building.

#include "builder/sol-ast/calls/SolInternalCall.h"
#include "builder/sol-types/RefParamPassing.h"
#include "builder/sol-ast/exprs/SolIndexAccess.h"
#include "builder/ProgramAnalysis.h"
#include "builder/CallTarget.h"
#include "builder/sol-ast/EvmSlotLowering.h"
#include "builder/sol-ast/MappingPrefix.h"
#include "builder/storage/EvmLayoutMode.h"
#include "awst/NameGen.h"
#include "builder/sol-types/SolIntType.h"
#include "builder/AWSTBuilder.h"
#include "builder/sol-ast/EffectScan.h"
#include "builder/sol-ast/AsmScan.h"
#include "builder/sol-ast/StorageRefPointer.h"
#include "builder/itxn/AsaIntrinsics.h"
#include "builder/abi/Arc4Stdlib.h"
#include "builder/itxn/CallResolver.h"
#include "builder/itxn/FunctionPointerBuilder.h"
#include "builder/sol-types/FunctionPointerKind.h"
#include "builder/sol-types/OverloadSuffix.h"
#include "builder/sol-types/TypeMapper.h"
#include "builder/sol-types/Arc4Defaults.h"
#include "builder/assembly/AssemblyBuilder.h"
#include "builder/sol-types/TypeCoercion.h"
#include "builder/sol-types/ConversionPlan.h"
#include "builder/sol-eb/AssignmentHelper.h"
#include "builder/storage/StorageMapper.h"
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

int64_t enclosingCallableId(Context const& context)
{
	for (auto const* scope = &context; scope; scope = scope->parent())
		if (auto const* function = dynamic_cast<FunctionContext const*>(scope))
			return function->callableId;
	return 0;
}

bool syntaxReferencesFunction(FunctionCall const& call)
{
	Expression const* callee = &call.expression();
	if (auto const* options = dynamic_cast<FunctionCallOptions const*>(callee))
		callee = &options->expression();
	if (auto const* tuple = dynamic_cast<TupleExpression const*>(callee);
		tuple && tuple->components().size() == 1 && tuple->components()[0])
		callee = tuple->components()[0].get();
	Declaration const* declaration = nullptr;
	if (auto const* identifier = dynamic_cast<Identifier const*>(callee))
		declaration = identifier->annotation().referencedDeclaration;
	else if (auto const* member = dynamic_cast<MemberAccess const*>(callee))
		declaration = member->annotation().referencedDeclaration;
	return dynamic_cast<FunctionDefinition const*>(declaration) != nullptr;
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
	auto const& runtimeKey = scope.findMappingKeyParam(path.declaration->id());
	if (!runtimeKey.empty())
		return awst::makeVarExpression(
		runtimeKey, awst::WType::bytesType(), loc);
	if (path.declaration->isStateVariable())
	{
		auto binding = ctx.storageMapper.physicalBindingFor(*path.declaration);
		if (binding.kind == awst::AppStorageKind::Box)
			return awst::makeUtf8BytesConstant(
				binding.name, loc, awst::WType::bytesType());
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
				sr.rootBox = awst::makeBoxValueExpression(box->key, box->wtype, box->sourceLocation);
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
	std::vector<StorageRoot> const& roots,
	std::vector<size_t> const& memoryRefParamIndices,
	CallBoundaryPlan const& plan,
	awst::SourceLocation const& m_loc)
{
	// AWSTBuilder augments return type when storage/memory-ref params exist:
	//   non-void: (r0..rK-1, sp0..spN-1, mp0..mpM-1) — original return
	//     FLATTENED (K values, not nested WTuple).
	//   void: bare type if N+M==1; tuple otherwise.
	// Always unpack (even unresolved args) — wtype mismatch otherwise.
	auto* origRetType = call->wtype;
	bool voidReturn = (origRetType == awst::WType::voidType());
	auto const* origRetTuple = voidReturn
		? nullptr
		: dynamic_cast<awst::WTuple const*>(origRetType);

	size_t origRetCount = voidReturn
		? 0
		: (origRetTuple ? origRetTuple->types().size() : 1);

	// 1 element → bare type (puya doesn't wrap single-elem returns);
	// 2+ → WTuple.
	auto const* callTupleType = plan.augmentReturn(ctx.typeMapper, origRetType);
	call->wtype = callTupleType;

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

	// Memory-ref writeback: assign post-call tuple slot back to caller
	// local (VarExpression). Skip non-VarExpression args (no stable
	// lvalue without re-evaluating side-effecting bases).
	size_t memBaseIdx = baseIdx + roots.size();
	for (size_t mi = 0; mi < memoryRefParamIndices.size(); ++mi)
	{
		size_t pi = memoryRefParamIndices[mi];
		auto const* argVar = dynamic_cast<awst::VarExpression const*>(
			call->args[pi].value.get());
		// A non-VarExpression arg (a temporary like `mut(getArray())`) has
		// no caller-visible lvalue to write back to — the mutation is
		// unobservable anyway (EVM matches). Correctly dropped, no warning.
		if (!argVar || argVar->name.empty())
			continue;

		auto* memArgType = call->args[pi].value->wtype;
		auto modifiedArg = pickFromTuple(memBaseIdx + mi, memArgType);

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

void SolInternalCall::collectSubroutineParamTypes(
	FunctionDefinition const& _funcDef,
	std::vector<awst::WType const*>& paramTypes,
	std::set<size_t>& mappingStorageParamIndices,
	std::set<size_t>& evmSlotRefParamIndices,
	std::set<size_t>& blobOffsetParamIndices)
{
	auto const& plan = m_ctx.typeMapper.callBoundaryPlan(_funcDef, m_ctx.currentContract);
	for (auto const& parameter: plan.parameters) paramTypes.push_back(parameter.type);
	mappingStorageParamIndices = plan.keyParams;
	evmSlotRefParamIndices = plan.slotParams;
	blobOffsetParamIndices = plan.blobParams;
}

// For a mapping/storage-ref param: extract the box-key prefix;
// callee uses it for box key derivation.
std::shared_ptr<awst::Expression> SolInternalCall::extractMappingKeyPrefix(
	Expression const& argExpr)
{
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
	// (`_pools ++ hash(id)`), not a static name (all keys would alias).
	// Build the element access, lift its box key; callee reinterprets it.
	if (dynamic_cast<IndexAccess const*>(&argExpr))
	{
		auto built = awst::unwrapStateGet(buildExpr(argExpr));
		if (auto const* box = dynamic_cast<awst::BoxValueExpression const*>(built.get()))
			return awst::makeReinterpretCast(
				box->key, awst::WType::bytesType(), m_loc);
	}

	std::string name;
	if (auto const* ident = dynamic_cast<Identifier const*>(&argExpr))
	{
		name = ident->name();
		// If registered as a mapping-key ref, runtime box-key lives in the
		// variable's VALUE — e.g. V4 `pool` holds sha256(id++"_pools").
		// Reading the var keys on the real element. Unregistered bare state
		// mappings fall through to the constant-name prefix.
		if (auto const* d = ident->annotation().referencedDeclaration;
			d && !m_scope.findMappingKeyParam(d->id()).empty())
			return awst::makeVarExpression(name, awst::WType::bytesType(), m_loc);
		// A storage-ref LOCAL (`P storage allowed = allowance[a][b][c];`)
		// carries its element's box key in its ALIAS — the shared
		// resolver lifts it, INCLUDING any field names the alias walked
		// (the old inline peel handled ReinterpretCast wrappers but
		// dropped FieldExpression names; the direct-access peel had the
		// opposite gap — MappingPrefix.h). The name fallback below
		// literally named a box after the local, so every entry the
		// callee wrote through such a param COLLAPSED into one shared
		// "allowed" box (Permit2's allowance).
		if (auto const* d = ident->annotation().referencedDeclaration)
			if (m_scope.findStorageAlias(d->id()))
				if (auto holder = sol_ast::resolveHolderRoot(
						m_ctx, m_scope, argExpr, m_loc))
					return awst::makeReinterpretCast(
						std::move(holder), awst::WType::bytesType(), m_loc);
	}
	else if (auto const* ma = dynamic_cast<MemberAccess const*>(&argExpr))
	{
		// Full-depth field-chain derivation shared with direct access
		// (resolveCursorContext): root holder ++ utf8(f) per level. The
		// old inline version resolved DEPTH-1 only, so `f(st.a.m)` keyed
		// bare utf8("m") while st.a.m[k] keyed utf8(st)++"a"++"m" —
		// split-brain state between direct and ref-param access.
		if (auto prefix = sol_ast::resolveMappingHolderPrefix(
				m_ctx, m_scope, argExpr, m_loc))
			return awst::makeReinterpretCast(
				std::move(prefix), awst::WType::bytesType(), m_loc);
		name = ma->memberName();
	}
	if (name.empty())
		name = "map"; // fallback
	return awst::makeUtf8BytesConstant(name, m_loc);
}

void SolInternalCall::buildSequencedArgs(
	std::shared_ptr<awst::SubroutineCallExpression> const& call,
	FunctionDefinition const* _funcDef,
	bool _isUsingForCall,
	std::vector<awst::WType const*> const& paramTypes,
	std::set<size_t> const& mappingStorageParamIndices,
	std::set<size_t> const& evmSlotRefParamIndices,
	std::set<size_t> const& blobOffsetParamIndices)
{
	// Args evaluate left-to-right on EVM (verified vs 0.8.20 + py-evm), with
	// each arg's write-backs landing before the NEXT arg — and before the call
	// itself executes. Capture each arg's queued effects; re-emitted in order
	// below once all args are built.
	std::vector<eb::ContractContext::OperandDeltas> argDeltas;
	std::vector<bool> argMayWrite;
	auto const* plan = _funcDef ? &m_ctx.typeMapper.callBoundaryPlan(*_funcDef, m_ctx.currentContract) : nullptr;
	std::map<size_t, std::shared_ptr<awst::Expression>> offsets;
	auto keyArgument = [&](Expression const& expression, size_t pi) {
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
			argMayWrite.push_back(builder::EffectScan::mayWrite(memberAccess->expression(), m_ctx, m_scope));
			call->args.push_back(std::move(ca));
		}
	}

	// Build arguments with type coercion
	auto const sortedArgs = m_call.sortedArguments();
	for (size_t i = 0; i < sortedArgs.size(); ++i)
	{
		awst::CallArg ca;
		size_t paramIdx = _isUsingForCall ? (i + 1) : i;
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
			// Slot mode: a storage-ref arg bound to a VALUE (memory) param
			// materializes here — the slot handle can't coerce to the value
			// type (it crashed field reads: "extraction end 8 beyond length").
			if (v && paramIdx < paramTypes.size())
				v = sol_ast::EvmSlotLowering::materializeRefValue(
					m_ctx, m_scope, std::move(v),
					sortedArgs[i]->annotation().type,
					paramTypes[paramIdx], m_loc);
			if (_funcDef && paramIdx < _funcDef->parameters().size()
				&& paramIdx < paramTypes.size())
				v = builder::ConversionPlan{
					sortedArgs[i]->annotation().type,
					_funcDef->parameters()[paramIdx]->type(),
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
		argMayWrite.push_back(builder::EffectScan::mayWrite(*sortedArgs[i], m_ctx, m_scope));
		call->args.push_back(std::move(ca));
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
			bool laterEffects = false;
			for (size_t aj = ai + 1; aj < argDeltas.size(); ++aj)
				laterEffects = laterEffects
					|| !argDeltas[aj].empty() || argMayWrite[aj];
			bool pin = (laterEffects || !argDeltas[ai].post.empty())
				&& call->args[ai].value
				&& call->args[ai].value->wtype
				&& call->args[ai].value->wtype->immutable();
			call->args[ai].value = m_ctx.emitSequencedOperand(
				std::move(argDeltas[ai]), std::move(call->args[ai].value), pin, m_loc);
		}
	}
	if (plan)
		for (auto pi: plan->offsetParams)
			call->args.push_back({std::nullopt, offsets.at(pi)});
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
				if (auto offVar = m_scope.findStructRefOffset(vd->id());
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
						int elemSize = builder::computeEncodedElementSize(elemArc4);
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

	ParameterMutationSummary const* mutations = nullptr;
	if (_funcDef)
	{
		// The source can name a base declaration while the selected method is
		// an override with different write-back requirements and parameter names.
		// Use the same solc lookup as mutation analysis before planning its ABI.
		auto const& analysis = m_ctx.typeMapper.analysis();
		FunctionDefinition const* caller = nullptr;
		if (auto found = analysis.functionDeclarations.find(enclosingCallableId(m_scope));
			found != analysis.functionDeclarations.end())
			caller = found->second;
		if (auto const* concrete = resolveReferenceCallTarget(m_ctx.currentContract, caller, m_call))
		{
			_funcDef = concrete;
			_returnType = returnTypeFrom(concrete);
		}
		mutations = m_ctx.typeMapper.analysis().parameterMutationsForCall(
			m_ctx.currentContract, enclosingCallableId(m_scope), m_call);
		// A locally resolved function-pointer target is not visible in the call
		// expression's solc declaration. In that case `_funcDef` is already the
		// exact implementation selected by the translation scope.
		if (!mutations && !syntaxReferencesFunction(m_call))
			mutations = &m_ctx.typeMapper.analysis().parameterMutations(
				m_ctx.currentContract, *_funcDef);
	}
	auto call = awst::makeSubroutineCall(std::move(_target), _returnType, m_loc);

	// Collect param types for coercion; detect mapping storage-ref params.
	std::vector<awst::WType const*> paramTypes;
	std::set<size_t> mappingStorageParamIndices;
	std::set<size_t> evmSlotRefParamIndices;
	std::set<size_t> blobOffsetParamIndices;
	if (_funcDef)
		collectSubroutineParamTypes(
			*_funcDef, paramTypes, mappingStorageParamIndices,
			evmSlotRefParamIndices, blobOffsetParamIndices);

	buildSequencedArgs(
		call, _funcDef, _isUsingForCall, paramTypes,
		mappingStorageParamIndices, evmSlotRefParamIndices,
		blobOffsetParamIndices);

	if (_funcDef)
		applyAliasingGuard(*call, _funcDef, mutations, m_loc);

	if (_funcDef)
	{
		auto const& plan = m_ctx.typeMapper.callBoundaryPlan(*_funcDef, m_ctx.currentContract);
		if (!plan.writeBackParams.empty())
		{
			auto roots = traceStorageRoots(*call, plan.storageWriteBackParams);
			auto origRet = emitAugmentedCallWriteBacks(
				m_ctx, call, roots, plan.memoryWriteBackParams, plan, m_loc);
			return wrapStorageRefResult(std::move(origRet), _funcDef);
		}
		auto const* target = std::get_if<awst::InstanceMethodTarget>(&call->target);
		if (_funcDef->isPartOfExternalInterface() && target
			&& target->memberName == eb::CallResolver::resolveMethodName(m_ctx, *_funcDef))
		{
			for (size_t pi = 0; pi < plan.parameters.size(); ++pi)
			{
				auto const& parameter = plan.parameters[pi];
				call->args[pi].value = parameter.encodeArgument(std::move(call->args[pi].value), m_loc);
				call->args[pi].name = parameter.wireName();
			}
			call->wtype = m_ctx.typeMapper.functionReturnPlan(*_funcDef).wireType;
			return decodeCallResult(call, _returnType, m_loc);
		}
	}

	return wrapStorageRefResult(call, _funcDef);
}

std::shared_ptr<awst::Expression> SolInternalCall::resolveIdentifierCall(
	Identifier const& _ident)
{
	std::string name = _ident.name();
	auto const* decl = _ident.annotation().referencedDeclaration;

	// Check if this is a function pointer variable call
	if (auto const* varDecl = dynamic_cast<VariableDeclaration const*>(decl))
	{
		if (auto const* target = m_scope.findFuncPtrTarget(varDecl->id()))
		{
			decl = target;
			Logger::instance().debug("resolved function pointer '" + name + "' to '" + target->name() + "'");
		}
		else if (auto const* funcType = dynamic_cast<FunctionType const*>(varDecl->type()))
		{
			bool isInternal = funcType->kind() == FunctionType::Kind::Internal;
			bool isExternal = isExternalFunctionPointer(funcType);

			if (isInternal || isExternal)
			{
				awst::WType const* ptrWType = isInternal
					? awst::WType::uint64Type()
					: m_ctx.typeMapper.map(funcType);

				std::shared_ptr<awst::Expression> ptrExpr;
				if (varDecl->isStateVariable())
				{
					// Slot mode: the write lowered to a slot, so the read must
					// too (absent slot reads 0 → uninitialised-call panic, same
					// as EVM). typeMapper maps FunctionType to ptrWType exactly.
					if (m_ctx.typeMapper.profile().evmStorageLayout && !varDecl->isConstant()
						&& !varDecl->immutable()
						&& varDecl->referenceLocation()
							!= VariableDeclaration::Location::Transient)
					{
						EvmSlotLowering low(m_ctx, m_scope, m_loc);
						auto addr = low.addrForStateVar(*varDecl);
						if (!addr)
							return nullptr;
						ptrExpr = low.readValue(*addr);
					}
					else
						ptrExpr = m_ctx.storageMapper.createStateRead(
							name, ptrWType,
							awst::AppStorageKind::AppGlobal, m_loc);
				}
				else
				{
					// Read local by mangled AWST name (name__<declId>; bare reads
					// unassigned var → puya "used before assignment").
					auto var = awst::makeVarExpression(
						m_scope.awstVarName(*varDecl), ptrWType, m_loc);
					ptrExpr = std::move(var);
				}

				std::vector<std::shared_ptr<awst::Expression>> args;
				for (auto const& arg : m_call.arguments())
					args.push_back(m_ctx.buildExpr(*arg));

				auto result = eb::FunctionPointerBuilder::buildFunctionPointerCall(
					m_ctx, std::move(ptrExpr), funcType, std::move(args), m_loc);
				if (result)
					return result;
			}

			// Fallback for unsupported kinds:
			// emit assert(false) to revert (matches EVM behavior for uninitialized pointers)
			Logger::instance().warning(
				"call to function pointer '" + name + "' (state var / unsupported), emitting assert(false)", m_loc);
			m_ctx.queuePostExpression(awst::makeAssert(
				awst::makeFalse(m_loc), m_loc, "uninitialized function pointer"), m_loc);

			auto vc = awst::makeVoidConstant(m_loc);
			return vc;
		}
	}

	if (auto const* funcDef = dynamic_cast<FunctionDefinition const*>(decl))
	{
		auto* retType = returnTypeFrom(funcDef);
		awst::SubroutineTarget target;

		// MRO super dispatch: if the target is registered as a super stub,
		// use InstanceMethodTarget(stub) so fn-ptr-bound `x()` where
		// `x = super.f` doesn't bypass the f__super_<callerId> stub.
		if (auto superName = m_scope.findSuperTarget(funcDef->id()); !superName.empty())
		{
			target = awst::InstanceMethodTarget{std::move(superName)};
			return buildSubroutineCall(std::move(target), retType, funcDef, false);
		}

		// Try library/free function resolution via CallResolver
		auto resolved = eb::CallResolver::resolveFromIdentifier(
			m_ctx, _ident, eb::CallResolver::resolveMethodName(m_ctx, *funcDef));
		if (resolved)
		{
			target = resolved->target;
		}
		else
		{
			// Regular instance method
			target = awst::InstanceMethodTarget{eb::CallResolver::resolveMethodName(m_ctx, *funcDef)};
		}

		return buildSubroutineCall(std::move(target), retType, funcDef, false);
	}

	// Unknown identifier — fallback
	auto* retType = m_ctx.typeMapper.map(m_call.annotation().type);
	return buildSubroutineCall(
		awst::InstanceMethodTarget{name}, retType, nullptr, false);
}

std::shared_ptr<awst::Expression> SolInternalCall::resolveMemberAccessCall(
	MemberAccess const& _memberAccess)
{
	auto* retType = m_ctx.typeMapper.map(m_call.annotation().type);

	// `this.x()` for signed int ≤64-bit state var: auto-getter returns
	// biguint (sign-extension). Match that wtype here.
	if (auto const* refDecl = _memberAccess.annotation().referencedDeclaration)
	{
		if (auto const* varDecl = dynamic_cast<VariableDeclaration const*>(refDecl))
		{
			if (varDecl->isStateVariable() && !varDecl->isConstant())
			{
				auto const* solType = varDecl->type();
				if (auto const* udvt =
					dynamic_cast<solidity::frontend::UserDefinedValueType const*>(solType))
					solType = &udvt->underlyingType();
				if (auto const* intType =
					dynamic_cast<solidity::frontend::IntegerType const*>(solType))
					if (intType->isSigned() && intType->numBits() <= 64)
						retType = awst::WType::biguintType();
			}
		}
	}

	FunctionDefinition const* resolvedFuncDef = nullptr;
	bool isUsingForCall = false;

	// ARC4's abi.encode envelope must be intercepted before normal argument
	// lowering; the nested abi.encode supplies Solidity type information only.
	if (auto arc4Result = eb::Arc4Stdlib::tryHandleCall(
			m_ctx, _memberAccess, m_call, m_loc))
		return *arc4Result;

	// AVM stdlib intrinsic intercept: short-circuits library resolution so the
	// fail-fast bodies in libs/AVM.sol are never used as runtime subroutines.
	if (auto asaResult = eb::AsaIntrinsics::tryHandleCall(
			m_ctx, _memberAccess, m_call, m_loc))
		return *asaResult;

	// Try CallResolver first (handles library, free, using-for, super)
	auto resolved = eb::CallResolver::resolveFromMemberAccess(
		m_ctx, m_scope, _memberAccess,
		_memberAccess.memberName(), m_call.arguments().size());
	if (resolved)
	{
		resolvedFuncDef = resolved->funcDef;
		if (resolvedFuncDef)
			retType = returnTypeFrom(resolvedFuncDef);
		return buildSubroutineCall(
			resolved->target, retType, resolvedFuncDef, resolved->isUsingForCall);
	}

	// Check base type for super/base internal calls
	auto const* baseType = _memberAccess.expression().annotation().type;
	bool wasTypeType = false;
	if (baseType && baseType->category() == Type::Category::TypeType)
	{
		wasTypeType = true;
		auto const* typeType = dynamic_cast<TypeType const*>(baseType);
		if (typeType) baseType = typeType->actualType();
	}

	if (baseType && baseType->category() == Type::Category::Contract)
	{
		auto const* contractType = dynamic_cast<ContractType const*>(baseType);

		// Base internal call: BaseContract.method() or super.method()
		if (wasTypeType && contractType)
		{
			auto const* refDecl = _memberAccess.annotation().referencedDeclaration;
			if (auto const* funcDef = dynamic_cast<FunctionDefinition const*>(refDecl))
			{
				resolvedFuncDef = funcDef;
				retType = returnTypeFrom(funcDef);

				// Check if there's a __super_N subroutine for this base function
				if (auto superName = m_scope.findSuperTarget(funcDef->id()); !superName.empty())
				{
					auto target = awst::InstanceMethodTarget{std::move(superName)};
					return buildSubroutineCall(std::move(target), retType, funcDef, false);
				}

				auto target = awst::InstanceMethodTarget{
					eb::CallResolver::resolveMethodName(m_ctx, *funcDef)};
				return buildSubroutineCall(std::move(target), retType, funcDef, false);
			}

			// Function pointer state variable: C.x() where x is function() internal
			if (auto const* varDecl = dynamic_cast<VariableDeclaration const*>(refDecl))
			{
				if (auto const* funcType = dynamic_cast<FunctionType const*>(varDecl->type()))
				{
					if (funcType->kind() == FunctionType::Kind::Internal)
					{
						std::shared_ptr<awst::Expression> ptrExpr;
						if (m_ctx.typeMapper.profile().evmStorageLayout && !varDecl->isConstant()
							&& !varDecl->immutable()
							&& varDecl->referenceLocation()
								!= VariableDeclaration::Location::Transient)
						{
							EvmSlotLowering low(m_ctx, m_scope, m_loc);
							auto addr = low.addrForStateVar(*varDecl);
							if (!addr)
								return nullptr;
							ptrExpr = low.readValue(*addr);
						}
						else
						{
							auto binding =
								m_ctx.storageMapper.physicalBindingFor(*varDecl);
							ptrExpr = m_ctx.storageMapper.createStateRead(
								binding.name, awst::WType::uint64Type(),
								binding.kind, m_loc);
						}

						std::vector<std::shared_ptr<awst::Expression>> args;
						for (auto const& arg : m_call.arguments())
							args.push_back(m_ctx.buildExpr(*arg));

						auto result = eb::FunctionPointerBuilder::buildFunctionPointerCall(
							m_ctx, std::move(ptrExpr), funcType, std::move(args), m_loc);
						if (result)
							return result;
					}
				}
			}
		}
	}

	// Last resort: try library/free function by AST ID
	auto const* refDecl = _memberAccess.annotation().referencedDeclaration;
	if (auto const* funcDef = dynamic_cast<FunctionDefinition const*>(refDecl))
	{
		resolvedFuncDef = funcDef;
		retType = returnTypeFrom(funcDef);

		// using-for (prepend receiver) vs direct `L.f(x, ...)` call?
		auto classifyUsingFor = [&]() -> bool {
			auto const* bt = _memberAccess.expression().annotation().type;
			if (!bt) return true;
			// `import "M" as N; N.f(x)` — N is a Module.
			if (bt->category() == Type::Category::Module) return false;
			// `L.f(x)` where L is a library/contract — TypeType referring to a contract.
			if (bt->category() == Type::Category::TypeType) return false;
			return true;
		};

		if (auto const* symbol = m_ctx.functionSymbols.resolve(funcDef->id()))
		{
			isUsingForCall = classifyUsingFor();
			return buildSubroutineCall(
				awst::SubroutineID{*symbol}, retType, funcDef, isUsingForCall);
		}
	}

	// Struct field fn-ptr `s.fn(...)`: InstanceMethodTarget{fn} would call
	// fn on current contract (wrong). Read the field → ARC4Decode if needed
	// → FunctionPointerBuilder dispatch.
	if (auto const* refDecl = _memberAccess.annotation().referencedDeclaration)
	{
		if (auto const* varDecl = dynamic_cast<VariableDeclaration const*>(refDecl))
		{
			auto const* funcType = dynamic_cast<FunctionType const*>(varDecl->type());
			bool isStructField = varDecl->scope()
				&& dynamic_cast<StructDefinition const*>(varDecl->scope());
			if (funcType && isStructField)
			{
				auto baseExpr = m_ctx.buildExpr(_memberAccess.expression());
				auto* ptrNativeType = eb::FunctionPointerBuilder::mapFunctionType(
					m_ctx, funcType);
				std::shared_ptr<awst::Expression> ptrExpr;
				if (baseExpr->wtype && baseExpr->wtype->kind() == awst::WTypeKind::ARC4Struct)
				{
					auto const* arc4Struct = dynamic_cast<awst::ARC4Struct const*>(baseExpr->wtype);
					awst::WType const* arc4FieldType = nullptr;
					for (auto const& [fname, ftype] : arc4Struct->fields())
						if (fname == _memberAccess.memberName())
						{
							arc4FieldType = ftype;
							break;
						}
					auto field = awst::makeFieldExpression(std::move(baseExpr), _memberAccess.memberName(), arc4FieldType ? arc4FieldType : ptrNativeType, m_loc);
					if (arc4FieldType && arc4FieldType != ptrNativeType)
					{
						auto decode = awst::makeARC4Decode(std::move(field), ptrNativeType, m_loc);
						ptrExpr = std::move(decode);
					}
					else
						ptrExpr = std::move(field);
				}
				else
				{
					auto field = awst::makeFieldExpression(std::move(baseExpr), _memberAccess.memberName(), ptrNativeType, m_loc);
					ptrExpr = std::move(field);
				}

				std::vector<std::shared_ptr<awst::Expression>> args;
				for (auto const& arg : m_call.arguments())
					args.push_back(m_ctx.buildExpr(*arg));

				auto result = eb::FunctionPointerBuilder::buildFunctionPointerCall(
					m_ctx, std::move(ptrExpr), funcType, std::move(args), m_loc);
				if (result)
					return result;
			}
		}
	}

	// Fallback: InstanceMethodTarget
	std::string methodName = _memberAccess.memberName();
	if (resolvedFuncDef)
		methodName = eb::CallResolver::resolveMethodName(m_ctx, *resolvedFuncDef);
	return buildSubroutineCall(
		awst::InstanceMethodTarget{methodName}, retType, resolvedFuncDef, false);
}

std::shared_ptr<awst::Expression> SolInternalCall::resolveFunctionPointerCast(
	FunctionCall const& _innerCall)
{
	if (_innerCall.arguments().size() == 1)
	{
		if (auto const* argId = dynamic_cast<Identifier const*>(_innerCall.arguments()[0].get()))
		{
			auto const* decl = argId->annotation().referencedDeclaration;
			if (auto const* targetFunc = dynamic_cast<FunctionDefinition const*>(decl))
			{
				auto* retType = m_ctx.typeMapper.map(m_call.annotation().type);
				auto target = awst::InstanceMethodTarget{
					eb::CallResolver::resolveMethodName(m_ctx, *targetFunc)};
				Logger::instance().debug(
					"resolved function pointer cast: calling '" + targetFunc->name() + "' directly");
				return buildSubroutineCall(std::move(target), retType, targetFunc, false);
			}
		}
	}

	Logger::instance().error("could not resolve function call target", m_loc);
	auto* retType = m_ctx.typeMapper.map(m_call.annotation().type);
	return buildSubroutineCall(
		awst::InstanceMethodTarget{"unknown"}, retType, nullptr, false);
}

std::shared_ptr<awst::Expression> SolInternalCall::toAwst()
{
	auto const& funcExpr = funcExpression();

	if (auto const* identifier = dynamic_cast<Identifier const*>(&funcExpr))
		return resolveIdentifierCall(*identifier);

	if (auto const* memberAccess = dynamic_cast<MemberAccess const*>(&funcExpr))
		return resolveMemberAccessCall(*memberAccess);

	// Generic fn-ptr call: evaluate expression to get pointer ID, dispatch.
	// Before the cast resolver so `x()()` (nested fn-ptr) dispatches correctly.
	{
		auto const* exprType = funcExpr.annotation().type;
		auto const* funcType = dynamic_cast<FunctionType const*>(exprType);
		if (funcType
			&& (funcType->kind() == FunctionType::Kind::Internal
				|| isExternalFunctionPointer(funcType)))
		{
			auto ptrExpr = m_ctx.buildExpr(funcExpr);
			auto* wantedType = eb::FunctionPointerBuilder::mapFunctionType(
				m_ctx, funcType);
			// Shape-compare (not pointer): TypeMapper/FunctionPointerBuilder
			// may create distinct BytesWType instances for the same shape.
			auto shapeMatches = [](awst::WType const* _a, awst::WType const* _b) {
				if (_a == _b) return true;
				if (!_a || !_b) return false;
				if (_a->kind() != _b->kind()) return false;
				if (_a->kind() == awst::WTypeKind::Bytes)
				{
					auto const* ab = static_cast<awst::BytesWType const*>(_a);
					auto const* bb = static_cast<awst::BytesWType const*>(_b);
					return ab->length() == bb->length();
				}
				return _a == _b;
			};
			if (ptrExpr && !shapeMatches(ptrExpr->wtype, wantedType))
			{
				// Coerce ARC4-encoded fn-ptr to its native profile-selected type.
				auto const* srcKind = ptrExpr->wtype;
				bool srcIsArc4 = srcKind
					&& (srcKind->kind() == awst::WTypeKind::ARC4UIntN
						|| srcKind->kind() == awst::WTypeKind::ARC4StaticArray);
				if (srcIsArc4)
				{
					auto decode = awst::makeARC4Decode(std::move(ptrExpr), wantedType, m_loc);
					ptrExpr = std::move(decode);
				}
			}
			if (ptrExpr && shapeMatches(ptrExpr->wtype, wantedType))
			{
				std::vector<std::shared_ptr<awst::Expression>> args;
				for (auto const& arg : m_call.arguments())
					args.push_back(m_ctx.buildExpr(*arg));

				auto result = eb::FunctionPointerBuilder::buildFunctionPointerCall(
					m_ctx, std::move(ptrExpr), funcType, std::move(args), m_loc);
				if (result)
					return result;
			}
		}
	}

	if (auto const* innerCall = dynamic_cast<FunctionCall const*>(&funcExpr))
		return resolveFunctionPointerCast(*innerCall);

	// Fallback: unresolvable call
	Logger::instance().error("could not resolve function call target", m_loc);
	auto* retType = m_ctx.typeMapper.map(m_call.annotation().type);
	return buildSubroutineCall(
		awst::InstanceMethodTarget{"unknown"}, retType, nullptr, false);
}

} // namespace puyasol::builder::sol_ast
