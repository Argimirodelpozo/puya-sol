/// @file SolInternalCall.cpp
/// Internal function call resolution and SubroutineCallExpression building.

#include "builder/ast/calls/SolInternalCall.h"
#include "builder/eb/ResolvedLValue.h"
#include "builder/eb/CalldataReference.h"
#include "builder/types/RefParamPassing.h"
#include "builder/ast/exprs/SolIndexAccess.h"
#include "builder/context/ProgramAnalysis.h"
#include "builder/solc/SolcFacts.h"
#include "builder/storage/named/StoragePathWalker.h"
#include "builder/storage/slot/EvmSlotLowering.h"
#include "builder/eb/MappingPrefix.h"
#include "builder/target/EvmLayoutMode.h"
#include "awst/NameGen.h"
#include "builder/types/SolIntType.h"
#include "builder/context/BuildArtifacts.h"
#include "builder/types/ReturnWirePlan.h"
#include "builder/solc/StorageRefPointer.h"
#include "builder/lowering/intrinsics/AsaIntrinsics.h"
#include "builder/lowering/itxn/ApplicationCall.h"
#include "builder/lowering/itxn/InnerCallHandlers.h"
#include "builder/lowering/abi/Arc4Stdlib.h"
#include "builder/lowering/calls/CallResolver.h"
#include "builder/lowering/calls/FunctionPointerBuilder.h"
#include "builder/types/FunctionPointerKind.h"
#include "builder/types/TypeMapper.h"
#include "builder/codec/Arc4Defaults.h"
#include "builder/codec/EvmMemoryCodec.h"
#include "builder/codec/SelectorSemantics.h"
#include "builder/types/TypeCoercion.h"
#include "builder/types/ConversionPlan.h"
#include "builder/storage/StorageMapper.h"
#include "builder/storage/StoragePlace.hpp"
#include "builder/yul/AssemblyBuilder.h"
#include "Logger.h"

#include <algorithm>
#include <functional>
#include <map>
#include <set>
#include <vector>

#include <libsolidity/ast/AST.h>

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
	auto const& plan = m_ctx.typeMapper.functionReturnPlan(*_funcDef);
	auto const* type = dynamic_cast<FunctionType const*>(m_call.expression().annotation().type);
	return type && type->kind() == FunctionType::Kind::External ? plan.nativeType : plan.internalType;
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
	auto const* cursor = &SolcFacts::functionExpression(expression);
	while (auto const* index = SolcFacts::expressionAs<IndexAccess>(cursor))
	{
		if (!index->indexExpression())
			return std::nullopt;
		result.indices.push_back(index);
		cursor = &SolcFacts::functionExpression(index->baseExpression());
	}
	result.root = SolcFacts::expressionAs<Identifier>(cursor);
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
	auto const& storageReturns = m_ctx.typeMapper.analysis().storageReturnFacts(_funcDef);
	auto const* indexAccess = storageReturns.indexedReturn;
	if (!indexAccess)
		return _result;
	// Box-keyed mapping-of-struct storage ref: the callee already returns the
	// bytes box-key prefix (see mapReturnType / the return-body handling). Pass
	// it through unchanged — the caller binds it as a struct-storage-ref
	// (SolVariableDeclaration) — rather than reconstituting an IndexExpression,
	// which here would be the invalid `bytes[idx] -> Struct`.
	if (storageReturns.bytesKeyed)
		return _result;
	auto base = m_ctx.buildExpr(indexAccess->baseExpression());
	auto* elemType = m_ctx.typeMapper.map(
		_funcDef->returnParameters()[0]->type());
	return awst::makeIndexExpression(
		std::move(base), std::move(_result), elemType, m_loc);
}

std::shared_ptr<awst::Expression> SolInternalCall::extractMappingKeyPrefix(
	Expression const& source)
{
	auto const& argExpr = SolcFacts::unparenthesized(source);
	if (containsMappingType(argExpr.annotation().type))
		return storageReferenceKey(m_ctx, m_scope, argExpr, m_loc);
	// Any-rank array element path rooted in one physical box keeps that
	// root key. A companion byte offset identifies the selected struct.
	if (auto path = boxedArrayPath(argExpr))
		if (auto key = boxedArrayKey(m_ctx, m_scope, *path, m_loc))
			return key;

	// Array element (`arr[i]`) passed as a struct ref (handle-model dual handle): the element
	// is a SLICE of the array's box, not its own box — lift the ARRAY's box key here; the
	// companion offset carries header + i*elemSize. Mapping values (`m[k]`)
	// ARE their own box and are handled by the generic lift below.
	if (auto const* iaArr = SolcFacts::expressionAs<IndexAccess>(&argExpr))
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
	if (SolcFacts::expressionAs<IndexAccess>(&argExpr))
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
	awst::SubroutineTarget const* target)
{
	auto const* plan = _funcDef ? &m_ctx.typeMapper.callBoundaryPlan(*_funcDef, m_ctx.currentContract) : nullptr;
	auto const* functionType = dynamic_cast<FunctionType const*>(funcExpression().annotation().type);
	auto const* methodTarget = target ? std::get_if<awst::InstanceMethodTarget>(target) : nullptr;
	bool const abiTarget = _funcDef && _funcDef->isPartOfExternalInterface() && methodTarget
		&& methodTarget->memberName == eb::CallResolver::resolveMethodName(m_ctx, *_funcDef);
	bool const frame = plan ? plan->calldataFrame && !abiTarget
		: functionType && m_ctx.typeMapper.analysis().pointerNeedsCalldata(*functionType);
	bool const libraryFrame = frame && functionType && functionType->kind() == FunctionType::Kind::DelegateCall;
	std::map<size_t, CalldataReference> references;
	std::vector<awst::WType const*> paramTypes;
	if (plan)
		for (auto const& parameter: plan->parameters)
			paramTypes.push_back(abiTarget && parameter.abiNativeType ? parameter.abiNativeType : parameter.type);
	else if (functionType)
		for (auto const* type: functionType->parameterTypes())
			paramTypes.push_back(m_ctx.typeMapper.map(type));
	static std::set<size_t> const noParameters;
	auto const& mappingStorageParamIndices = plan ? plan->keyParams : noParameters;
	auto const& evmSlotRefParamIndices = plan ? plan->slotParams : noParameters;
	auto const blobOffsetParamIndices = abiTarget ? noParameters : plan ? plan->blobParams
		: functionType ? m_ctx.typeMapper.analysis().pointerMemoryParameters(*functionType) : noParameters;
	for (auto pi: blobOffsetParamIndices) paramTypes.at(pi) = awst::WType::uint64Type();
	std::map<size_t, std::shared_ptr<awst::Expression>> offsets;
	auto keyArgument = [&](Expression const& source, size_t pi) -> std::shared_ptr<awst::Expression> {
		auto const& expression = SolcFacts::unparenthesized(source);
		auto const* array = dynamic_cast<ArrayType const*>(expression.annotation().type);
		bool largeFixed = isLargeFixedArrayRef(m_ctx.typeMapper, expression.annotation().type);
		// A key-only array parameter addresses the entire encoded box. An
		// interior dynamic array also needs parent offset-table/resize metadata;
		// passing just its parent's key silently writes the wrong array header.
		bool dynamicValue = array && !containsMappingType(array) && hasDynamicStorageShape(array);
		if (largeFixed || dynamicValue)
		{
			if (auto const* id = SolcFacts::expressionAs<Identifier>(&expression))
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
		if (plan && std::find(plan->offsetParams.begin(), plan->offsetParams.end(), pi) != plan->offsetParams.end())
		{
			auto [key, offset] = bindBoxedReference(expression);
			auto name = "__call_offset_" + std::to_string(awst::NameGen::next("SolInternalCall.offset"));
			auto variable = awst::makeVarExpression(name, awst::WType::uint64Type(), m_loc);
			m_ctx.preEffects().push_back(awst::makeAssignmentStatement(variable, std::move(offset), m_loc));
			offsets.emplace(pi, std::move(variable));
			return key;
		}
		return extractMappingKeyPrefix(expression);
	};

	auto bindArgument = [&](Expression const& expression, size_t paramIdx) -> std::shared_ptr<awst::Expression> {
		auto const& source = SolcFacts::unparenthesized(expression);
		auto const* parameterType = _funcDef && paramIdx < _funcDef->parameters().size()
			? _funcDef->parameters()[paramIdx]->type()
			: functionType && paramIdx < functionType->parameterTypes().size()
				? functionType->parameterTypes()[paramIdx] : nullptr;
		if (evmSlotRefParamIndices.count(paramIdx))
		{
			EvmSlotLowering low(m_ctx, m_scope, m_loc);
			auto address = low.resolve(source);
			return address ? address->slot : nullptr;
		}
		if (mappingStorageParamIndices.count(paramIdx))
			return keyArgument(source, paramIdx);
		if (blobOffsetParamIndices.count(paramIdx))
			return SolIndexAccess::buildMemoryReference(m_ctx, m_scope, source, parameterType, m_loc,
				functionType && functionType->kind() == FunctionType::Kind::DelegateCall);
		std::shared_ptr<awst::Expression> value;
		if (frame && !libraryFrame && parameterType && parameterType->dataStoredIn(DataLocation::CallData))
		{
			auto reference = CalldataReference::resolve(m_ctx, source, m_loc);
			if (!reference) throw SizeError("cannot transport this calldata reference across an internal call");
			reference->offset = m_ctx.emitSequencedOperand({}, reference->offset, true, m_loc);
			if (reference->length) reference->length = m_ctx.emitSequencedOperand({}, reference->length, true, m_loc);
			if (reference->data) reference->data = m_ctx.emitSequencedOperand({}, reference->data, true, m_loc);
			// The callee consumes coordinates, not a speculative copy of the
			// referent. In particular, forwarding a forged pointer must not
			// perform bounds checks when the callee only observes its length.
			value = TypeCoercion::makeDefaultValue(paramTypes.at(paramIdx), m_loc);
			references.emplace(paramIdx, std::move(*reference));
		}
		else if (plan && *source.annotation().isLValue
			&& plan->parameters[paramIdx].passing == RefParamPassing::Value
			&& std::find(plan->writeBackParams.begin(), plan->writeBackParams.end(), paramIdx)
				!= plan->writeBackParams.end())
		{
			// Capture the destination before value sequencing can replace a state
			// read with a temporary. Never reconstruct it from the call operand.
			auto destination = std::make_shared<ResolvedLValue>(m_ctx, source, m_loc);
			value = destination->read();
			m_writeBacks.emplace(paramIdx, std::move(destination));
		}
		else value = buildExpr(source);
		if (parameterType && !parameterType->dataStoredIn(DataLocation::Storage))
			value = StorageMapper::makePartialBoxReadWithDefault(
				m_ctx.typeMapper, std::move(value), m_ctx.preEffects(), m_loc);
		if (value && paramIdx < paramTypes.size())
			value = EvmSlotLowering::materializeRefValue(m_ctx, m_scope,
				std::move(value), source.annotation().type, paramTypes[paramIdx], m_loc);
		if (parameterType && paramIdx < paramTypes.size())
			return ConversionPlan{source.annotation().type, parameterType, paramTypes[paramIdx],
				isExternalFunctionPointer(functionType) ? ConversionPlan::Context::AbiArgument
					: ConversionPlan::Context::Argument}.emit(std::move(value), m_loc, &m_ctx.preEffects());
		return paramIdx < paramTypes.size()
			? TypeCoercion::coerceScalar(std::move(value), paramTypes[paramIdx], m_loc) : value;
	};
	auto values = CallOperands::buildParameters(m_ctx, m_call, m_loc, bindArgument);
	if (libraryFrame)
	{
		// Delegatecall encodes a new immutable input. Coordinates returned by
		// the library refer to this frame, never the caller's transaction input.
		std::vector<std::pair<std::string, awst::WType const*>> parameters;
		std::map<std::string, Type const*> solTypes;
		for (size_t i = 0; i < values.size(); ++i)
		{
			auto const* type = plan->parameters[i].declaration->type();
			auto value = values[i];
			if (blobOffsetParamIndices.contains(i)) value = materializeEvmMemoryValue(
				m_ctx.typeMapper, type, m_ctx.typeMapper.map(type), value, m_loc, m_ctx.preEffects());
			auto name = "__library_arg_" + std::to_string(awst::NameGen::next("SolInternalCall.calldata"));
			m_ctx.preEffects().push_back(awst::makeAssignmentStatement(
				awst::makeVarExpression(name, value->wtype, m_loc), value, m_loc));
			parameters.emplace_back(name, value->wtype);
			solTypes.emplace(name, type);
			if (type->dataStoredIn(DataLocation::CallData)) references.emplace(i, CalldataReference{type,
				awst::makeVarExpression("__cd_off_" + name, awst::WType::biguintType(), m_loc),
				CalldataReference::hasLength(type)
					? awst::makeVarExpression("__cd_len_" + name, awst::WType::biguintType(), m_loc) : nullptr});
		}
		auto blob = awst::makeVarExpression("__cd_blob", awst::WType::bytesType(), m_loc);
		auto saved = m_scope.function && m_scope.function->hasAssemblyCalldata
			? m_ctx.emitSequencedOperand({}, blob, true, m_loc) : nullptr;
		AssemblyBuilder assembly(m_ctx.typeMapper, m_ctx.sourceFile, "library calldata");
		assembly.setCalldataSolTypes(std::move(solTypes));
		assembly.prepareCalldata(parameters, m_ctx.preEffects(), m_loc, false);
		m_resultCalldataFrame = m_ctx.emitSequencedOperand({}, awst::makeReplace3(blob,
			awst::makeZero(m_loc), SelectorSemantics::functionSelector(m_ctx, *functionType,
				eb::InnerCallHandlers::buildMethodSelector(m_ctx, _funcDef), m_loc), m_loc), true, m_loc);
		if (saved) m_ctx.preEffects().push_back(awst::makeAssignmentStatement(blob, saved, m_loc));
	}
	for (auto& value: values)
		args.push_back({std::nullopt, std::move(value)});
	if (plan)
		for (auto pi: plan->offsetParams)
			args.push_back({std::nullopt, offsets.at(pi)});
	if (frame)
	{
		if (!libraryFrame && (!m_scope.function || !m_scope.function->hasAssemblyCalldata))
			throw SizeError("internal calldata consumer has no input frame");
		awst::pushCallArg(args, libraryFrame ? m_resultCalldataFrame
			: awst::makeVarExpression("__cd_blob", awst::WType::bytesType(), m_loc));
		for (auto const& [pi, reference]: references)
		{
			awst::pushCallArg(args, reference.offset);
			if (CalldataReference::hasLength(reference.type)) awst::pushCallArg(args, reference.length);
			awst::pushCallArg(args, libraryFrame ? m_resultCalldataFrame : reference.data);
		}
	}
}

std::pair<std::shared_ptr<awst::Expression>, std::shared_ptr<awst::Expression>>
SolInternalCall::bindBoxedReference(Expression const& source)
{
	auto const& argExpr = SolcFacts::unparenthesized(source);
	// A storage-ref PARAM passed onward carries ITS caller-supplied
	// runtime offset — forward the offset var (bump(s) inside
	// inner(S storage s) wrote element 0 without this).
	if (auto const* id = SolcFacts::expressionAs<Identifier>(&argExpr))
		if (auto const* vd = dynamic_cast<VariableDeclaration const*>(
				id->annotation().referencedDeclaration))
			if (auto offVar = m_scope.bindings.structRefOffsets.get(vd->id());
				!offVar.empty())
				return {extractMappingKeyPrefix(argExpr), awst::makeVarExpression(
					offVar, awst::WType::uint64Type(), m_loc)};
	if (auto path = boxedArrayPath(argExpr))
		if (auto key = boxedArrayKey(m_ctx, m_scope, *path, m_loc))
		{
			key = m_ctx.emitSequencedOperand({}, std::move(key), true, m_loc);
			auto const* rootW = m_ctx.typeMapper.map(path->declaration->type());
			auto boxKey = awst::makeReinterpretCast(
				key, awst::WType::boxKeyType(), m_loc);
			auto box = awst::makeBoxValueExpression(
				std::move(boxKey), rootW, m_loc);
			std::string bytesName = "__sref_path_" + std::to_string(
				awst::NameGen::next("SolInternalCall.structRefPath"));
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
					throw SizeError("unsupported boxed storage-reference path");
				auto idx = CallOperands::evaluate(m_ctx, *index->indexExpression(), m_loc);
				// Evaluate each index before observing the current length/offset
				// table: the index expression may have resized the enclosing box.
				m_ctx.preEffects().push_back(awst::makeAssignmentStatement(bytesVar(),
					awst::makeAsBytes(StorageMapper::makeStateGetWithDefault(box, rootW, m_loc), m_loc), m_loc));
				auto length = array->isDynamicallySized() ? awst::makeBtoi(awst::makeExtract3(
					bytesVar(), base, awst::makeIntegerConstant(2, m_loc), m_loc), m_loc) : nullptr;
				idx = StoragePathWalker::checkedArrayIndex(*array, std::move(idx),
					std::move(length), m_ctx.preEffects(), m_loc);
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
						throw SizeError("boxed storage-reference element has no fixed encoded size");
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
				base = m_ctx.emitSequencedOperand({}, std::move(base), true, m_loc);
				current = array->baseType();
			}
			return {key, base};
		}
	return {extractMappingKeyPrefix(argExpr), awst::makeZero(m_loc)}; // whole-box
}



std::shared_ptr<awst::Expression> SolInternalCall::buildSubroutineCall(
	awst::SubroutineTarget _target,
	awst::WType const* _returnType,
	FunctionDefinition const* _funcDef)
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
	// The public ABI remains unchanged; direct Solidity calls use a private
	// implementation carrier when reference results must travel back.
	if (plan && (!plan->writeBackParams.empty() || plan->calldataFrame || !plan->blobParams.empty()
		|| m_ctx.typeMapper.functionReturnPlan(*_funcDef).internalType != m_ctx.typeMapper.functionReturnPlan(*_funcDef).nativeType)
		&& _funcDef->isPartOfExternalInterface()
		&& functionType && functionType->kind() == FunctionType::Kind::Internal
		&& std::holds_alternative<awst::InstanceMethodTarget>(_target))
		_target = awst::InstanceMethodTarget{eb::CallResolver::baseImplementationName(m_ctx, *_funcDef)};
	auto const* target = std::get_if<awst::InstanceMethodTarget>(&_target);
	bool const abiEntry = _funcDef && _funcDef->isPartOfExternalInterface() && target
		&& target->memberName == eb::CallResolver::resolveMethodName(m_ctx, *_funcDef);
	auto const* emittedReturn = abiEntry
		? m_ctx.typeMapper.functionReturnPlan(*_funcDef).wireType
		: plan ? plan->augmentReturn(m_ctx.typeMapper, _returnType) : _returnType;
	auto call = awst::makeSubroutineCall(std::move(_target), emittedReturn, m_loc);

	buildSequencedArgs(call->args, _funcDef, &call->target);
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
	std::shared_ptr<awst::Expression> callResult = call;
	if (!abiEntry && m_scope.function && (!_funcDef
		|| m_ctx.typeMapper.analysis().callablesWithRawReturn.contains(_funcDef->id())))
		callResult = ApplicationCall::propagateRawReturn(m_ctx, std::move(callResult), m_loc);

	if (_funcDef)
	{
		auto const& plan = m_ctx.typeMapper.callBoundaryPlan(*_funcDef, m_ctx.currentContract);
		if (!abiEntry && !plan.writeBackParams.empty())
		{
			auto result = m_ctx.emitSequencedOperand({}, callResult, true, m_loc);
			auto [original, modified] = plan.unpackReturn(std::move(result), _returnType, m_loc);
			for (size_t i = 0; i < plan.writeBackParams.size(); ++i)
				if (auto destination = m_writeBacks.find(plan.writeBackParams[i]);
					destination != m_writeBacks.end())
				{
					auto writes = m_ctx.lowerOperand([&] {
						return destination->second->write(std::move(modified[i]));
					}, false);
					for (auto& effect: writes.effects.pre) m_ctx.queuePostEffect(std::move(effect));
					for (auto& effect: writes.effects.post) m_ctx.queuePostEffect(std::move(effect));
				}
			return wrapStorageRefResult(std::move(original), _funcDef);
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

	std::shared_ptr<awst::Expression> result = std::move(callResult);
	if (abiEntry && functionType && isExternalFunctionPointer(functionType))
	{
		result = ApplicationCall::withStaticContext(m_ctx.typeMapper, std::move(result),
			functionType->stateMutability() <= StateMutability::View, m_loc, m_ctx.preEffects());
		result = ApplicationCall::decodeRawReturn(m_ctx.typeMapper, std::move(result),
			functionType->returnParameterTypes(), m_loc, m_ctx.preEffects());
		if (!functionType->returnParameterTypes().empty())
			result = awst::makeSingleEvaluation(std::move(result), emittedReturn, awst::nextSingleEvalId(), m_loc);
		ApplicationCall::setTypedReturnData(m_ctx.typeMapper, result,
			functionType->returnParameterTypes(), m_ctx.typeMapper.profile().contractAbi == ContractAbi::Evm,
			m_loc, m_ctx.preEffects());
		if (functionType->returnParameterTypes().empty()) return awst::makeVoidConstant(m_loc);
	}
	return wrapStorageRefResult(decodeCallResult(std::move(result), _returnType, m_loc), _funcDef);
}

std::shared_ptr<awst::Expression> SolInternalCall::resolveIdentifierCall(
	Identifier const& identifier)
{
	if (auto resolved = eb::CallResolver::resolveFunction(m_ctx, identifier))
		return buildSubroutineCall(std::move(resolved->target),
			returnTypeFrom(resolved->funcDef), resolved->funcDef);
	return buildSubroutineCall(awst::InstanceMethodTarget{identifier.name()},
		m_ctx.typeMapper.map(m_call.annotation().type), nullptr);
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
			returnTypeFrom(resolved->funcDef), resolved->funcDef);

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
		{
			auto const* result = abiReturnNativeType(m_ctx.typeMapper, type);
			wire.push_back(m_ctx.typeMapper.profile().contractAbi == ContractAbi::Evm ? result
				: planReturnElement(m_ctx.typeMapper, type, result).wireType);
		}
		auto* resultType = wire.size() == 1 ? wire.front()
			: m_ctx.typeMapper.createType<awst::WTuple>(std::move(wire));
		auto call = awst::makeSubroutineCall(
			awst::InstanceMethodTarget{member.memberName()}, resultType, m_loc);
		buildSequencedArgs(call->args, nullptr);
		for (size_t i = 0; i < call->args.size(); ++i)
		{
			auto const* type = getter->parameterTypes()[i];
			CallParameterPlan parameter;
			parameter.type = m_ctx.typeMapper.map(type);
			parameter.setAbiWireType(m_ctx.typeMapper, type);
			call->args[i].value = parameter.encodeArgument(std::move(call->args[i].value), m_loc);
		}
		auto result = awst::makeSingleEvaluation(std::move(call), resultType, awst::nextSingleEvalId(), m_loc);
		ApplicationCall::setTypedReturnData(m_ctx.typeMapper, result, getter->returnParameterTypes(),
			m_ctx.typeMapper.profile().contractAbi == ContractAbi::Evm, m_loc, m_ctx.preEffects());
		return decodeCallResult(std::move(result), native, m_loc);
	}
	return buildSubroutineCall(
		awst::InstanceMethodTarget{member.memberName()}, native, nullptr);
}

std::shared_ptr<awst::Expression> SolInternalCall::buildFunctionPointerCall(
	Expression const& callee, FunctionType const& type)
{
	// Retain the direct-call reference conventions only for immutable source
	// facts. A mutable local must read its runtime pointer, even before the
	// first syntactically encountered write or through a branch/loop.
	if (auto const* identifier = SolcFacts::expressionAs<Identifier>(&callee);
		identifier && SolcFacts::callOptions(m_call.expression()).empty())
		if (auto const* declaration = identifier->annotation().referencedDeclaration)
		{
			auto const& stable = m_ctx.typeMapper.analysis().stableFunctionPointers;
			if (auto it = stable.find(declaration->id()); it != stable.end())
			{
				auto const& initializer = SolcFacts::functionExpression(*it->second);
				auto const* member = SolcFacts::expressionAs<MemberAccess>(&initializer);
				// A foreign receiver remains an actual inner application call.
				if (type.kind() == FunctionType::Kind::Internal || (member && SolcFacts::isThis(member->expression())))
					if (auto resolved = eb::CallResolver::resolveFunction(m_ctx, initializer))
						return buildSubroutineCall(std::move(resolved->target),
							returnTypeFrom(resolved->funcDef), resolved->funcDef);
			}
		}

	// Identifier/member/index readers own storage and ARC4 field decoding.
	// Capture effects before emitting either operand group: legacy solc
	// evaluates internal-call arguments before its callee; IR and external
	// calls evaluate the callee first.
	auto pointer = m_ctx.lower(callee, false);
	auto const* wanted = m_ctx.typeMapper.map(&type);
	if (pointer.value && !awst::structurallyEquivalent(pointer.value->wtype, wanted))
		pointer.value = decodeCallResult(std::move(pointer.value), wanted, m_loc);
	bool const calleeFirst = m_ctx.viaIRSequencing || isExternalFunctionPointer(&type);
	auto emitPointer = [&]() {
		pointer.value = m_ctx.emitSequencedOperand(std::move(pointer.effects),
			std::move(pointer.value), true, m_loc);
	};
	if (calleeFirst) emitPointer();
	auto callValue = extractCallValue();
	std::vector<awst::CallArg> arguments;
	// Even a plain pointer read can observe a change made by an argument's
	// value expression (not just its queued effects). Finish args first on legacy.
	buildSequencedArgs(arguments, nullptr);
	if (!calleeFirst) emitPointer();
	std::vector<std::shared_ptr<awst::Expression>> values;
	for (auto& argument: arguments)
		values.push_back(std::move(argument.value));
	auto result = eb::FunctionPointerBuilder::buildFunctionPointerCall(
		m_ctx, std::move(pointer.value), &type, std::move(values), m_loc, std::move(callValue));
	if (type.kind() == FunctionType::Kind::Internal && m_scope.function)
		result = ApplicationCall::propagateRawReturn(m_ctx, std::move(result), m_loc);
	return result;
}

std::shared_ptr<awst::Expression> SolInternalCall::toAwst()
{
	auto result = toReferenceAwst();
	auto const* type = dynamic_cast<FunctionType const*>(m_call.expression().annotation().type);
	return result && type
		? materializeReferenceResult(m_ctx.typeMapper, type->returnParameterTypes(), std::move(result), m_loc,
			m_ctx.preEffects())
		: result;
}

bool SolInternalCall::hasReferenceReturns(FunctionCall const& call)
{
	auto const* type = dynamic_cast<FunctionType const*>(call.expression().annotation().type);
	if (!type || type->kind() != FunctionType::Kind::Internal) return false;
	for (auto const* result: type->returnParameterTypes())
		if (!result->isValueType() && (result->dataStoredIn(DataLocation::Memory)
			|| result->dataStoredIn(DataLocation::CallData))) return true;
	return false;
}

std::shared_ptr<awst::Expression> SolInternalCall::toReferenceAwst()
{
	auto const plan = eb::CallResolver::plan(m_call);
	if (plan.isFunctionPointer && plan.functionType)
		return buildFunctionPointerCall(*plan.callee, *plan.functionType);
	if (auto const* identifier = SolcFacts::expressionAs<Identifier>(plan.callee))
		return resolveIdentifierCall(*identifier);
	if (auto const* member = SolcFacts::expressionAs<MemberAccess>(plan.callee))
		return resolveMemberAccessCall(*member);
	Logger::instance().error("could not resolve function call target", m_loc);
	return buildSubroutineCall(awst::InstanceMethodTarget{"unknown"},
		m_ctx.typeMapper.map(m_call.annotation().type), nullptr);
}

} // namespace puyasol::builder::sol_ast
