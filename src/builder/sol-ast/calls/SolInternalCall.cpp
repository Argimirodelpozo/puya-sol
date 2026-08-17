/// @file SolInternalCall.cpp
/// Internal function call resolution and SubroutineCallExpression building.
/// Migrated from FunctionCallBuilder.cpp lines 3324-4390.

#include "builder/sol-ast/calls/SolInternalCall.h"
#include "builder/ProgramAnalysis.h"
#include "builder/sol-ast/EvmSlotLowering.h"
#include "builder/storage/EvmLayoutMode.h"
#include "awst/NameGen.h"
#include "builder/sol-types/SolIntType.h"
#include "builder/AWSTBuilder.h"
#include "builder/sol-ast/EffectScan.h"
#include "builder/sol-ast/AsmScan.h"
#include "builder/sol-ast/StorageRefPointer.h"
#include "builder/itxn/AsaIntrinsics.h"
#include "builder/itxn/CallResolver.h"
#include "builder/itxn/FunctionPointerBuilder.h"
#include "builder/sol-types/FunctionPointerKind.h"
#include "builder/sol-types/OverloadSuffix.h"
#include "builder/sol-types/TypeMapper.h"
#include "builder/sol-types/Arc4Defaults.h"
#include "builder/assembly/AssemblyBuilder.h"
#include "builder/sol-types/TypeCoercion.h"
#include "builder/sol-types/ConversionPlan.h"
#include "builder/storage/StorageMapper.h"
#include "Logger.h"

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
	if (_funcDef->returnParameters().empty())
		return awst::WType::voidType();

	// Unwrap UDVT / enum to locate a signed integer type for biguint promotion.
	// ContractBuilder upgrades signed int ≤64 bit returns to biguint so sign
	// extension works — but only at the ABI boundary (public/external). For
	// private/internal callees the target retains its native uint64 return,
	// so the call-site wtype must match.
	bool isAbiBoundary = _funcDef->isPartOfExternalInterface();
	auto mapReturnType = [&](solidity::frontend::Type const* solType) -> awst::WType const* {
		auto* mapped = m_ctx.typeMapper.map(solType);
		auto intInfo = builder::SolIntType::fromSolOrEnum(solType);
		if (intInfo && intInfo->isSigned && intInfo->bits <= 64 && isAbiBoundary)
			return awst::WType::biguintType();
		return mapped;
	};

	if (_funcDef->returnParameters().size() == 1)
	{
		// --evm-storage-layout: ANY storage ref return is a biguint slot.
		if (m_ctx.typeMapper.profile().evmStorageLayout
			&& _funcDef->returnParameters()[0]->referenceLocation()
				== VariableDeclaration::Location::Storage)
			return awst::WType::biguintType();
		// Storage reference return with .slot assembly → biguint (slot number)
		if (_funcDef->returnParameters()[0]->referenceLocation() == VariableDeclaration::Location::Storage
			&& m_ctx.typeMapper.analysis().callablesWithInlineAssembly.count(
				_funcDef->id()))
			return awst::WType::biguintType();
		// Storage-ref pointer: subroutine returns uint64 index;
		// buildSubroutineCall wraps in IndexExpression to reconstitute the ref.
		if (builder::storageRefPointerReturn(_funcDef))
			// Box-keyed (e.g. `return _pools[id]` → `Pool.State storage`):
			// returns bytes box-key prefix; array/slot refs return uint64 index.
			return builder::storageRefReturnIsBytesKeyed(_funcDef)
				? awst::WType::bytesType()
				: awst::WType::uint64Type();
		// Blob-backed >4KB memory return: subroutine returns the uint64 base
		// offset (pointer model); the caller binds it (SolVariableDeclaration).
		{
			auto const& rp0 = _funcDef->returnParameters()[0];
			if (!rp0->name().empty()
				&& rp0->referenceLocation() == VariableDeclaration::Location::Memory
				&& builder::memoryUsesBlob(mapReturnType(rp0->type())))
				return awst::WType::uint64Type();
		}
		return mapReturnType(_funcDef->returnParameters()[0]->type());
	}

	std::vector<awst::WType const*> retTypes;
	for (auto const& param: _funcDef->returnParameters())
	{
		if (m_ctx.typeMapper.profile().evmStorageLayout
			&& param->referenceLocation() == VariableDeclaration::Location::Storage)
			retTypes.push_back(awst::WType::biguintType());   // slot handle
		else
			retTypes.push_back(mapReturnType(param->type()));
	}
	return m_ctx.typeMapper.createType<awst::WTuple>(std::move(retTypes), std::nullopt);
}

std::shared_ptr<awst::Expression> SolInternalCall::buildSubroutineCall(
	awst::SubroutineTarget _target,
	awst::WType const* _returnType,
	FunctionDefinition const* _funcDef,
	bool _isUsingForCall)
{
	// Storage-ref pointer function: the subroutine returns the uint64
	// index of the location. Reconstitute the storage reference at the
	// call site as `IndexExpression(<stateVar>, <call>)` — a real lvalue
	// node, which puya accepts where a SubroutineCallExpression would not.
	auto wrapStorageRef =
		[&](std::shared_ptr<awst::Expression> _result) -> std::shared_ptr<awst::Expression>
	{
		// --evm-storage-layout: the biguint slot IS the reference — no
		// IndexExpression reconstitution.
		if (m_ctx.typeMapper.profile().evmStorageLayout)
			return _result;
		auto const* indexAccess = builder::storageRefPointerReturn(_funcDef);
		if (!indexAccess)
			return _result;
		// Box-keyed mapping-of-struct storage ref: the callee already returns the
		// bytes box-key prefix (see mapReturnType / the return-body handling). Pass
		// it through unchanged — the caller binds it as a struct-storage-ref
		// (SolVariableDeclaration) — rather than reconstituting an IndexExpression,
		// which here would be the invalid `bytes[idx] -> Struct`.
		if (builder::storageRefReturnIsBytesKeyed(_funcDef))
			return _result;
		auto base = m_ctx.buildExpr(indexAccess->baseExpression());
		auto* elemType = m_ctx.typeMapper.map(
			_funcDef->returnParameters()[0]->type());
		return awst::makeIndexExpression(
			std::move(base), std::move(_result), elemType, m_loc);
	};

	// External fn-ptr params use the profile-selected dual-purpose byte layout;
	// dispatch handles them.

	auto call = awst::makeSubroutineCall(std::move(_target), _returnType, m_loc);
	ParameterMutationSummary const* mutations = nullptr;
	if (_funcDef)
	{
		mutations = m_ctx.typeMapper.analysis().parameterMutationsForCall(
			m_ctx.currentContract, enclosingCallableId(m_scope), m_call);
		// A locally resolved function-pointer target is not visible in the call
		// expression's solc declaration. In that case `_funcDef` is already the
		// exact implementation selected by the translation scope.
		if (!mutations && !syntaxReferencesFunction(m_call))
			mutations = &m_ctx.typeMapper.analysis().parameterMutations(
				m_ctx.currentContract, *_funcDef);
	}

	// Collect param types for coercion; detect mapping storage-ref params.
	std::vector<awst::WType const*> paramTypes;
	std::set<size_t> mappingStorageParamIndices;
	std::set<size_t> evmSlotRefParamIndices;
	if (_funcDef)
	{
		// Struct storage-ref params used via `.slot` in asm travel as a box-key
		// handle (mirror buildFreestandingSubroutine); pass the arg's box key.
		auto slotParams = builder::structRefParamsUsedAsAsmSlot(*_funcDef);
		for (size_t pi = 0; pi < _funcDef->parameters().size(); ++pi)
		{
			auto const& param = _funcDef->parameters()[pi];
			if (m_ctx.typeMapper.profile().evmStorageLayout
				&& param->referenceLocation() == VariableDeclaration::Location::Storage)
			{
				// --evm-storage-layout: pass the biguint slot of the argument.
				paramTypes.push_back(awst::WType::biguintType());
				evmSlotRefParamIndices.insert(pi);
			}
			else if (param->referenceLocation() == VariableDeclaration::Location::Storage
				&& (builder::isBoxKeyedStorageRef(
						param->type(), m_ctx.typeMapper.analysis())
					|| slotParams.count(pi))) // widened: plain structs + asm .slot refs
			{
				paramTypes.push_back(awst::WType::bytesType());
				mappingStorageParamIndices.insert(pi);
			}
			else if (param->referenceLocation() == VariableDeclaration::Location::Memory
				&& builder::memoryUsesBlob(m_ctx.typeMapper.map(param->type())))
				// Blob-backed (>4KB) memory aggregate: the callee receives the
				// uint64 base offset (pointer model), not the struct value. The
				// argument `p` resolves to its offset local (SolIdentifier).
				paramTypes.push_back(awst::WType::uint64Type());
			else
				paramTypes.push_back(m_ctx.typeMapper.map(param->type()));
		}
	}

	// For a mapping storage-ref param: extract the box-key prefix;
	// callee uses it for box key derivation.
	auto extractMappingKeyPrefix = [&](Expression const& argExpr)
		-> std::shared_ptr<awst::Expression>
	{
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
			// carries its element's box key in its ALIAS — lift it. The
			// name fallback below literally named a box after the local, so
			// every entry the callee wrote through such a param COLLAPSED
			// into one shared "allowed" box (Permit2's allowance).
			if (auto const* d = ident->annotation().referencedDeclaration)
				if (auto const* alias = m_scope.findStorageAlias(d->id()))
				{
					auto e = awst::unwrapStateGet(alias->expr);
					while (auto const* rc =
						dynamic_cast<awst::ReinterpretCast const*>(e.get()))
						e = rc->expr;
					if (auto const* box =
							dynamic_cast<awst::BoxValueExpression const*>(e.get());
						box && box->key)
						return awst::makeReinterpretCast(
							box->key, awst::WType::bytesType(), m_loc);
				}
		}
		else if (auto const* ma = dynamic_cast<MemberAccess const*>(&argExpr))
		{
			// `self.field` (registered mappingKeyParam): prefix = base runtime
			// box-key ++ utf8(field) — mirrors self.field[k] (resolveCursorContext)
			// so V4 `self.positions.get(k)` keys under the same box as direct access.
			if (auto const* baseId = dynamic_cast<Identifier const*>(&ma->expression()))
			{
				if (auto const* d = baseId->annotation().referencedDeclaration;
					d && !m_scope.findMappingKeyParam(d->id()).empty())
					return awst::makeReinterpretCast(
						awst::makeConcat(
							awst::makeVarExpression(
								baseId->name(), awst::WType::bytesType(), m_loc),
							awst::makeUtf8BytesConstant(ma->memberName(), m_loc),
							m_loc),
						awst::WType::bytesType(), m_loc);
				// `st.field` where st is a struct STATE VAR: utf8(st) ++
				// utf8(field) — matches resolveCursorContext's state-var-struct
				// branch, so a passed `st.m` keys the same boxes as direct
				// st.m[k]. Plain utf8(field) aliased every same-typed struct
				// state var.
				if (auto const* vd = dynamic_cast<VariableDeclaration const*>(
						baseId->annotation().referencedDeclaration);
					vd && vd->isStateVariable() && !vd->isConstant() && !vd->immutable())
					return awst::makeReinterpretCast(
						awst::makeConcat(
							awst::makeUtf8BytesConstant(baseId->name(), m_loc),
							awst::makeUtf8BytesConstant(ma->memberName(), m_loc),
							m_loc),
						awst::WType::bytesType(), m_loc);
			}
			name = ma->memberName();
		}
		if (name.empty())
			name = "map"; // fallback
		return awst::makeUtf8BytesConstant(name, m_loc);
	};

	// Args evaluate left-to-right on EVM (verified vs 0.8.20 + py-evm), with
	// each arg's write-backs landing before the NEXT arg — and before the call
	// itself executes. Capture each arg's queued effects; re-emitted in order
	// below once all args are built.
	std::vector<eb::ContractContext::OperandDeltas> argDeltas;
	std::vector<bool> argMayWrite, argLocalPure;

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
					return extractMappingKeyPrefix(memberAccess->expression());
				auto v = buildExpr(memberAccess->expression());
				if (!paramTypes.empty())
					v = builder::TypeCoercion::implicitNumericCast(
						std::move(v), paramTypes[0], m_loc);
				return v;
			}, /*_conditional=*/false);
			ca.value = std::move(lowered.value);
			argDeltas.push_back(std::move(lowered.effects));
			argMayWrite.push_back(builder::EffectScan::mayWrite(memberAccess->expression()));
			argLocalPure.push_back(builder::onlyLocalPure(memberAccess->expression()));
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
				return extractMappingKeyPrefix(*sortedArgs[i]);
			auto v = buildExpr(*sortedArgs[i]);
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
		argMayWrite.push_back(builder::EffectScan::mayWrite(*sortedArgs[i]));
		argLocalPure.push_back(builder::onlyLocalPure(*sortedArgs[i]));
		call->args.push_back(std::move(ca));
	}

	// Re-emit captured arg effects in arg order. With no write-backs and no
	// direct-state-writing args this restores the pre-statements
	// byte-identically. Otherwise each arg's write-backs hoist to pre-position
	// (so later args and the callee observe them), and an earlier arg whose
	// value a LATER arg's effects could disturb is pinned first (locals-only
	// values are immune and skip it). Mutable-wtype values are never pinned —
	// a pin temp would defeat the aliasing guard below.
	{
		for (size_t ai = 0; ai < argDeltas.size(); ++ai)
		{
			bool laterEffects = false;
			for (size_t aj = ai + 1; aj < argDeltas.size(); ++aj)
				laterEffects = laterEffects
					|| !argDeltas[aj].post.empty() || argMayWrite[aj];
			bool pin = laterEffects && !argLocalPure[ai]
				&& call->args[ai].value
				&& call->args[ai].value->wtype
				&& call->args[ai].value->wtype->immutable();
			call->args[ai].value = m_ctx.emitSequencedOperand(
				std::move(argDeltas[ai]), std::move(call->args[ai].value), pin, m_loc);
		}
	}

	// Handle-model dual handle: append a uint64 OFFSET arg for each offset-convention struct-ref
	// param (FunctionBuilder declared the matching `name__off` param). Array-element args (`arr[i]`)
	// pass the element byte offset (len header + i*elemSize); whole-box args pass 0. Order mirrors
	// the param iteration so caller/callee align by position.
	if (_funcDef)
	{
		auto offsetForArg = [&](Expression const* argExpr) -> std::shared_ptr<awst::Expression> {
			if (auto const* ia = dynamic_cast<IndexAccess const*>(argExpr))
				if (ia->indexExpression())
					if (auto const* at = dynamic_cast<ArrayType const*>(
							ia->baseExpression().annotation().type))
						if (!at->isByteArrayOrString() && at->baseType()
							&& at->baseType()->category() == Type::Category::Struct)
						{
							int elemSize = builder::computeEncodedElementSize(
								m_ctx.typeMapper.mapSolTypeToARC4(at->baseType()));
							if (elemSize > 0)
							{
								uint64_t header = at->isDynamicallySized() ? 2 : 0;
								auto idx = builder::TypeCoercion::implicitNumericCast(
									buildExpr(*ia->indexExpression()), awst::WType::uint64Type(), m_loc);
								return awst::makeUInt64BinOp(
									awst::makeUInt64BinOp(std::move(idx),
										awst::UInt64BinaryOperator::Mult,
										awst::makeIntegerConstant(
											static_cast<uint64_t>(elemSize), m_loc), m_loc),
									awst::UInt64BinaryOperator::Add,
									awst::makeIntegerConstant(header, m_loc), m_loc);
							}
						}
			return awst::makeIntegerConstant(0, m_loc); // whole-box → offset 0
		};
		for (size_t pi = 0; pi < _funcDef->parameters().size(); ++pi)
		{
			if (!m_ctx.typeMapper.analysis().structRefOffsetParams.count(
					_funcDef->parameters()[pi]->id()))
				continue;
			Expression const* argExpr = nullptr;
			if (_isUsingForCall)
			{
				if (pi >= 1 && (pi - 1) < sortedArgs.size())
					argExpr = sortedArgs[pi - 1].get();
			}
			else if (pi < sortedArgs.size())
				argExpr = sortedArgs[pi].get();
			awst::CallArg offCa;
			offCa.value = offsetForArg(argExpr);
			call->args.push_back(std::move(offCa));
		}
	}

	// Aliasing guard: same variable in >1 arg position → puya rejects
	// ("mutable values cannot be passed more than once", e.g. `s.concat(s)`).
	// Break alias with Copy only when safe: valid iff NONE of the aliased params
	// are mutated by the callee (e.g. stringutils' concat). If mutated, EVM
	// aliasing is live → leave the puya error rather than silently wrong-lower.
	if (_funcDef)
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
		for (size_t ai = 0; ai < call->args.size(); ++ai)
		{
			auto& ca = call->args[ai];
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
				auto& ca = call->args[positions[k]];
				auto copy = std::make_shared<awst::Copy>();
				copy->sourceLocation = m_loc;
				copy->wtype = ca.value->wtype;
				copy->value = std::move(ca.value);
				ca.value = std::move(copy);
			}
		}
	}

	// Storage write-back: AWSTBuilder augments non-private, non-pure/view
	// library/free functions to thread the modified storage arg back as
	// `WTuple(R, T)` (or bare `T` when R is void). Contract methods are
	// NOT augmented (direct storage access). Shapes:
	//   1. Box-backed (StateGet→BoxValueExpression), optional FieldExpression.
	//   2. Direct AppStateExpression (small struct state vars).
	bool calleeIsLibrary = false;
	bool calleeIsPrivate = false;
	bool calleeIsFree = false;
	if (_funcDef)
	{
		calleeIsPrivate = _funcDef->visibility() == Visibility::Private;
		calleeIsFree = _funcDef->isFree();
		if (auto const* contractDef = _funcDef->annotation().contract)
			calleeIsLibrary = contractDef->isLibrary();
	}
	// Collect storage param indices (mapping-type refs handled separately;
	// order must match AWSTBuilder.cpp:388-403 — source-order parameters()).
	std::vector<size_t> storageParamIndices;
	if (_funcDef
		&& !m_ctx.typeMapper.profile().evmStorageLayout   // slot handles write through — no write-back
		&& ((calleeIsLibrary && !calleeIsPrivate) || calleeIsFree)
		&& _funcDef->stateMutability() != StateMutability::View
		&& _funcDef->stateMutability() != StateMutability::Pure)
	{
		// Same asm-.slot widening as the box-key param type (above / AWSTBuilder):
		// such params travel as a box-key handle and write directly to the box, so
		// they get NO write-back slot — the callee is void, and a write-back
		// assignment of a void call asserts in puya.
		auto wbSlotParams = builder::structRefParamsUsedAsAsmSlot(*_funcDef);
		for (size_t pi = 0; pi < _funcDef->parameters().size() && pi < call->args.size(); ++pi)
		{
			auto const& p = _funcDef->parameters()[pi];
			if (p->referenceLocation() != VariableDeclaration::Location::Storage)
				continue;
			// Exclude box-keyed refs (mappings, mapping-carrying structs/arrays,
			// plain structs like `Pool.State`): travel as bytes key-prefix,
			// write directly to box, no write-back slot. Must match callee
			// predicate (AWSTBuilder.cpp `isBoxKeyedStorageRef`) or arity diverges.
			if (builder::isBoxKeyedStorageRef(
					p->type(), m_ctx.typeMapper.analysis()) || wbSlotParams.count(pi)) // widened: plain structs + asm .slot refs
				continue;
			storageParamIndices.push_back(pi);
		}
	}

	// Memory-ref params: same library/free scope; `pure` NOT excluded
	// (Solidity pure can mutate memory). Callee returns post-call value
	// as extra tuple slot; write back to caller local. Order must match
	// AWSTBuilder.cpp memoryRefParamIndices (storage first, then memory),
	// same use-def filter (only mutated params augmented).
	std::vector<size_t> memoryRefParamIndices;
	// Internal contract methods are now augmented too (FunctionBuilder), matching library/free.
	bool calleeIsInternalMethod = _funcDef && !calleeIsLibrary && !calleeIsFree
		&& _funcDef->visibility() == Visibility::Internal;
	if (_funcDef && _funcDef->isImplemented()
		&& ((calleeIsLibrary && !calleeIsPrivate) || calleeIsFree || calleeIsInternalMethod))
	{
		auto isMemRefType = [](Type const* t) {
			if (auto const* arr = dynamic_cast<ArrayType const*>(t))
				return !arr->isByteArrayOrString();
			return dynamic_cast<StructType const*>(t) != nullptr;
		};
		for (size_t pi = 0; pi < _funcDef->parameters().size() && pi < call->args.size(); ++pi)
		{
			auto const& p = _funcDef->parameters()[pi];
			if (p->referenceLocation() != VariableDeclaration::Location::Memory)
				continue;
			if (!p->type() || !isMemRefType(p->type()))
				continue;
			if (!mutations || !mutations->mutates(pi))
				continue;  // read-only — callee didn't augment it either
			memoryRefParamIndices.push_back(pi);
		}
	}

	if (!storageParamIndices.empty() || !memoryRefParamIndices.empty())
	{
		// Per-storage-arg root tracing. Each storage arg may resolve to a
		// different root (one might be `box.field`, another `appState`,
		// another a plain stack value with no resolvable root).
		struct StorageRoot {
			size_t paramIdx = 0;
			std::shared_ptr<awst::BoxValueExpression> rootBox;
			std::shared_ptr<awst::AppStateExpression> rootAppState;
			std::vector<std::string> fieldPath;
			awst::WType const* rootType = nullptr;
			awst::WType const* storageArgType = nullptr;
		};
		std::vector<StorageRoot> roots;
		roots.reserve(storageParamIndices.size());

		for (size_t pi: storageParamIndices)
		{
			StorageRoot sr;
			sr.paramIdx = pi;
			sr.storageArgType = call->args[pi].value->wtype;

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
			traceToRoot(call->args[pi].value.get());
			sr.rootType = sr.rootBox ? sr.rootBox->wtype
				: sr.rootAppState ? sr.rootAppState->wtype : nullptr;
			roots.push_back(std::move(sr));
		}

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

		std::vector<awst::WType const*> tupleTypes;
		if (!voidReturn)
		{
			if (origRetTuple)
				for (auto const* t: origRetTuple->types()) tupleTypes.push_back(t);
			else
				tupleTypes.push_back(origRetType);
		}
		for (auto const& sr: roots) tupleTypes.push_back(sr.storageArgType);
		for (size_t pi: memoryRefParamIndices)
			tupleTypes.push_back(call->args[pi].value->wtype);
		size_t origRetCount = voidReturn
			? 0
			: (origRetTuple ? origRetTuple->types().size() : 1);

		// 1 element → bare type (puya doesn't wrap single-elem returns);
		// 2+ → WTuple.
		awst::WType const* callTupleType =
			tupleTypes.size() == 1 ? tupleTypes[0]
				: m_ctx.typeMapper.createType<awst::WTuple>(std::move(tupleTypes));
		call->wtype = callTupleType;

		std::string tempName = "__storage_wb_" + std::to_string(awst::NameGen::next("SolInternalCall.storageWriteBackCounter"));

		auto tempVar = awst::makeVarExpression(tempName, callTupleType, m_loc);

		auto assignTemp = awst::makeAssignmentStatement(
			tempVar, std::shared_ptr<awst::Expression>(call), m_loc);
		m_ctx.preEffects().push_back(std::move(assignTemp));

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
			auto fieldPath = sr.fieldPath;
			std::reverse(fieldPath.begin(), fieldPath.end());

			std::shared_ptr<awst::Expression> writeValue = modifiedArg;
			if (!fieldPath.empty())
			{
				if (fieldPath.size() == 1)
				{
					auto const* structType =
						dynamic_cast<awst::ARC4Struct const*>(sr.rootType);
					if (structType)
					{
						std::shared_ptr<awst::Expression> readStruct;
						if (sr.rootBox)
						{
							readStruct = builder::StorageMapper::makeStateGetWithDefault(
								sr.rootBox, sr.rootType, m_loc);
						}
						else
						{
							readStruct = sr.rootAppState;
						}

						auto newStruct = awst::makeStructWithReplacedField(
							structType, readStruct, fieldPath[0], modifiedArg, m_loc);
						writeValue = std::move(newStruct);
					}
					else
					{
						// Reached only for a param the mutation detector flagged
						// as mutated, so dropping the write-back is a guaranteed
						// silent miscompile — fail loud instead.
						Logger::instance().error(
							"callee mutates a field of a non-struct storage-ref "
							"argument, which cannot be written back on AVM — the "
							"mutation would be silently lost. Restructure so the "
							"mutated root is a struct, or pass the field directly.",
							m_loc);
						writeValue = nullptr;
					}
				}
				else
				{
					Logger::instance().error(
						"callee mutates a nested (>1-deep) field of a storage-ref "
						"argument (only single-level field write-back is "
						"supported); the mutation would be silently lost. Pass the "
						"inner reference directly (e.g. `f(s.inner)` not `f(s)`).",
						m_loc);
					writeValue = nullptr;
				}
			}

			if (writeValue)
			{
				std::shared_ptr<awst::Expression> writeTarget =
					sr.rootBox ? std::static_pointer_cast<awst::Expression>(sr.rootBox)
							: std::static_pointer_cast<awst::Expression>(sr.rootAppState);

				auto writeBack = awst::makeAssignmentExpression(
					std::move(writeTarget), std::move(writeValue), m_loc, sr.rootType);

				m_ctx.queuePostExpression(std::move(writeBack), m_loc);
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

			m_ctx.queuePostExpression(std::move(writeBack), m_loc);
		}

		return wrapStorageRef(origRet);
	}

	return wrapStorageRef(call);
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

	// AVM stdlib intrinsic intercept: short-circuits library resolution for
	// `AVM.asaCreate / asaBalance / asaTotalSupply / asaTransfer` so the
	// stub bodies in tokens/AVM.sol never need to compile.
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
