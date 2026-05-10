/// @file SolInternalCall.cpp
/// Internal function call resolution and SubroutineCallExpression building.
/// Migrated from FunctionCallBuilder.cpp lines 3324-4390.

#include "builder/sol-ast/calls/SolInternalCall.h"
#include "builder/sol-eb/AsaIntrinsics.h"
#include "builder/sol-eb/CallResolver.h"
#include "builder/sol-eb/FunctionPointerBuilder.h"
#include "builder/sol-types/TypeMapper.h"
#include "builder/sol-types/TypeCoercion.h"
#include "builder/storage/StorageMapper.h"
#include "Logger.h"

#include <libsolidity/ast/AST.h>
#include <libsolidity/ast/ASTVisitor.h>

namespace puyasol::builder::sol_ast
{

using namespace solidity::frontend;

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
		auto const* t = solType;
		if (auto const* udvt = dynamic_cast<UserDefinedValueType const*>(t))
			t = &udvt->underlyingType();
		auto const* intType = dynamic_cast<IntegerType const*>(t);
		if (!intType)
			if (auto const* enumType = dynamic_cast<EnumType const*>(t))
				intType = dynamic_cast<IntegerType const*>(enumType->encodingType());
		if (intType && intType->isSigned() && intType->numBits() <= 64 && isAbiBoundary)
			return awst::WType::biguintType();
		return mapped;
	};

	if (_funcDef->returnParameters().size() == 1)
	{
		// Storage reference return with .slot assembly → biguint (slot number)
		if (_funcDef->returnParameters()[0]->referenceLocation() == VariableDeclaration::Location::Storage
			&& _funcDef->isImplemented()
			&& std::any_of(_funcDef->body().statements().begin(), _funcDef->body().statements().end(),
				[](auto const& s) { return dynamic_cast<InlineAssembly const*>(s.get()); }))
			return awst::WType::biguintType();
		return mapReturnType(_funcDef->returnParameters()[0]->type());
	}

	std::vector<awst::WType const*> retTypes;
	for (auto const& param: _funcDef->returnParameters())
		retTypes.push_back(mapReturnType(param->type()));
	return m_ctx.typeMapper.createType<awst::WTuple>(std::move(retTypes), std::nullopt);
}

std::shared_ptr<awst::Expression> SolInternalCall::buildSubroutineCall(
	awst::SubroutineTarget _target,
	awst::WType const* _returnType,
	FunctionDefinition const* _funcDef,
	bool _isUsingForCall)
{
	// External function-type params are passed as bytes (12-byte packed
	// appId + selector). No guard needed — the dispatch handles them.

	auto call = awst::makeSubroutineCall(std::move(_target), _returnType, m_loc);

	// Collect parameter types for coercion + detect mapping storage-ref params
	std::vector<awst::WType const*> paramTypes;
	std::set<size_t> mappingStorageParamIndices;
	if (_funcDef)
	{
		for (size_t pi = 0; pi < _funcDef->parameters().size(); ++pi)
		{
			auto const& param = _funcDef->parameters()[pi];
			if (param->referenceLocation() == VariableDeclaration::Location::Storage
				&& dynamic_cast<MappingType const*>(param->type()))
			{
				paramTypes.push_back(awst::WType::bytesType());
				mappingStorageParamIndices.insert(pi);
			}
			else
				paramTypes.push_back(m_ctx.typeMapper.map(param->type()));
		}
	}

	// Helper: for a mapping storage-ref param, extract the state variable
	// name from the argument expression and return it as a BytesConstant
	// key prefix. The callee uses this prefix for box key derivation.
	auto extractMappingKeyPrefix = [&](Expression const& argExpr)
		-> std::shared_ptr<awst::Expression>
	{
		std::string name;
		if (auto const* ident = dynamic_cast<Identifier const*>(&argExpr))
			name = ident->name();
		else if (auto const* ma = dynamic_cast<MemberAccess const*>(&argExpr))
			name = ma->memberName();
		if (name.empty())
			name = "map"; // fallback
		return awst::makeUtf8BytesConstant(name, m_loc);
	};

	// For using-for calls, prepend receiver as first arg
	if (_isUsingForCall)
	{
		auto const& funcExpr = funcExpression();
		if (auto const* memberAccess = dynamic_cast<MemberAccess const*>(&funcExpr))
		{
			awst::CallArg ca;
			if (mappingStorageParamIndices.count(0))
				ca.value = extractMappingKeyPrefix(memberAccess->expression());
			else
			{
				ca.value = buildExpr(memberAccess->expression());
				if (!paramTypes.empty())
					ca.value = builder::TypeCoercion::implicitNumericCast(
						std::move(ca.value), paramTypes[0], m_loc);
			}
			call->args.push_back(std::move(ca));
		}
	}

	// Build arguments with type coercion
	auto const sortedArgs = m_call.sortedArguments();
	for (size_t i = 0; i < sortedArgs.size(); ++i)
	{
		awst::CallArg ca;
		size_t paramIdx = _isUsingForCall ? (i + 1) : i;
		if (mappingStorageParamIndices.count(paramIdx))
			ca.value = extractMappingKeyPrefix(*sortedArgs[i]);
		else
		{
			ca.value = buildExpr(*sortedArgs[i]);
			if (paramIdx < paramTypes.size())
				ca.value = builder::TypeCoercion::implicitNumericCast(
					std::move(ca.value), paramTypes[paramIdx], m_loc);
		}
		call->args.push_back(std::move(ca));
	}

	// Storage write-back for calls whose first parameter is a storage
	// reference. AWSTBuilder augments non-private, non-pure/view library
	// functions AND free functions to thread the modified storage arg
	// back through the return value as `WTuple(R, T)` (or just `T` when
	// R is void).
	//
	// Contract methods are NOT augmented — they access storage directly.
	// So the call-site unpack is scoped to library/free callees.
	//
	// Two receiver shapes are supported:
	//  1. Box-backed state (StateGet → BoxValueExpression), optionally with
	//     a single-level FieldExpression (`x.field.method(...)`).
	//  2. Direct AppStateExpression (`x.method(...)` where x is a non-box
	//     state variable — the common case for small struct state vars).
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
	// Collect ALL storage param indices (mapping-type storage refs are
	// handled elsewhere — they get a different threading scheme). Order
	// must match AWSTBuilder.cpp:388-403, which iterates parameters() in
	// source order and appends each storage param's type to the
	// augmented return tuple.
	std::vector<size_t> storageParamIndices;
	if (_funcDef
		&& ((calleeIsLibrary && !calleeIsPrivate) || calleeIsFree)
		&& _funcDef->stateMutability() != StateMutability::View
		&& _funcDef->stateMutability() != StateMutability::Pure)
	{
		for (size_t pi = 0; pi < _funcDef->parameters().size() && pi < call->args.size(); ++pi)
		{
			auto const& p = _funcDef->parameters()[pi];
			if (p->referenceLocation() != VariableDeclaration::Location::Storage)
				continue;
			if (dynamic_cast<MappingType const*>(p->type()))
				continue;
			storageParamIndices.push_back(pi);
		}
	}

	// Memory-ref params: same library/free scope as storage, but `pure`
	// is NOT excluded (Solidity pure can mutate memory params). The
	// callee returns the post-call memory value as an extra tuple slot;
	// here we capture it and write back to the caller's local. Order
	// must match AWSTBuilder.cpp's `memoryRefParamIndices` (storage
	// params first, then memory params), AND the same use-def filter:
	// only AUGMENT params actually mutated by the callee body. Walks
	// the same root-of-LHS analysis as MemoryParamMutationDetector in
	// AWSTBuilder.cpp.
	std::vector<size_t> memoryRefParamIndices;
	if (_funcDef && _funcDef->isImplemented()
		&& ((calleeIsLibrary && !calleeIsPrivate) || calleeIsFree))
	{
		auto isMemRefType = [](Type const* t) {
			if (auto const* arr = dynamic_cast<ArrayType const*>(t))
				return !arr->isByteArrayOrString();
			return dynamic_cast<StructType const*>(t) != nullptr;
		};
		// Use-def: collect params written in the callee body. Mirrors
		// AWSTBuilder.cpp's MemoryParamMutationDetector.
		struct Detector : public ASTConstVisitor {
			std::set<int64_t> paramIds;
			std::set<int64_t> mutated;
			bool visit(Assignment const& a) override
			{
				record(&a.leftHandSide());
				return true;
			}
			void record(Expression const* lhs)
			{
				while (true)
				{
					if (auto const* ix = dynamic_cast<IndexAccess const*>(lhs))
						{ lhs = &ix->baseExpression(); continue; }
					if (auto const* mb = dynamic_cast<MemberAccess const*>(lhs))
						{ lhs = &mb->expression(); continue; }
					if (auto const* tup = dynamic_cast<TupleExpression const*>(lhs))
					{
						for (auto const& c : tup->components())
							if (c) record(c.get());
						return;
					}
					break;
				}
				if (auto const* id = dynamic_cast<Identifier const*>(lhs))
				{
					auto const* d = id->annotation().referencedDeclaration;
					if (d && paramIds.count(d->id()))
						mutated.insert(d->id());
				}
			}
		};
		Detector detector;
		for (auto const& p : _funcDef->parameters())
			detector.paramIds.insert(p->id());
		_funcDef->body().accept(detector);
		for (size_t pi = 0; pi < _funcDef->parameters().size() && pi < call->args.size(); ++pi)
		{
			auto const& p = _funcDef->parameters()[pi];
			if (p->referenceLocation() != VariableDeclaration::Location::Memory)
				continue;
			if (!p->type() || !isMemRefType(p->type()))
				continue;
			if (!detector.mutated.count(p->id()))
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

		// AWSTBuilder always augments the function's return type when it
		// has any storage / memory-ref params:
		//   - non-void return:  (r0, r1, ..., rK-1, sp0, ..., spN-1, mp0, ..., mpM-1)
		//     where (r0..rK-1) is the FLATTENED original return (matching how
		//     AWSTBuilder builds it from each return param's mapped type).
		//     A multi-value return like `returns (Fr, Fr, Fr)` is K=3 here, NOT
		//     a single nested WTuple.
		//   - void return:      single bare type if N+M==1; tuple otherwise.
		// We MUST unpack every time, even if some args don't resolve to a
		// state root — otherwise the assignment-target wtype mismatches.
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

		// callTupleType: the wtype the augmented call returns.
		// 1 element → bare type (puya doesn't wrap a single-element return in
		//             a tuple). 2+ elements → WTuple wrapper.
		awst::WType const* callTupleType =
			tupleTypes.size() == 1 ? tupleTypes[0]
				: m_ctx.typeMapper.createType<awst::WTuple>(std::move(tupleTypes));
		call->wtype = callTupleType;

		static int storageWriteBackCounter = 0;
		std::string tempName = "__storage_wb_" + std::to_string(storageWriteBackCounter++);

		auto tempVar = std::make_shared<awst::VarExpression>();
		tempVar->sourceLocation = m_loc;
		tempVar->wtype = callTupleType;
		tempVar->name = tempName;

		auto assignTemp = awst::makeAssignmentStatement(
			tempVar, std::shared_ptr<awst::Expression>(call), m_loc);
		m_ctx.prePendingStatements.push_back(std::move(assignTemp));

		// pickFromTuple: read element `idx` of typed `ty` from tempVar.
		// In the single-element bare-type case (callTupleType == ty), tempVar
		// IS the value — no TupleItemExpression needed.
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
			// Multi-value return: rebuild the original tuple from the
			// flattened head of the augmented call (elements 0..K-1).
			auto reTuple = awst::makeTupleExpression(origRetType, m_loc);
			for (size_t i = 0; i < origRetCount; ++i)
				reTuple->items.push_back(pickFromTuple(i, origRetTuple->types()[i]));
			origRet = std::move(reTuple);
		}
		else
		{
			origRet = pickFromTuple(0, origRetType);
		}

		// Emit one writeback per storage arg that resolved to a state root.
		// Args that didn't resolve (plain locals from the caller) get no
		// writeback — they're forwarded copies, the call modified them but
		// there's no source-of-truth to update.
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
							auto sg = std::make_shared<awst::StateGet>();
							sg->sourceLocation = m_loc;
							sg->wtype = sr.rootType;
							sg->field = sr.rootBox;
							sg->defaultValue =
								builder::StorageMapper::makeDefaultValue(sr.rootType, m_loc);
							readStruct = std::move(sg);
						}
						else
						{
							readStruct = sr.rootAppState;
						}

						auto newStruct = awst::makeNewStruct(structType, m_loc);
						for (auto const& [fn, ft]: structType->fields())
						{
							if (fn == fieldPath[0])
								newStruct->values[fn] = modifiedArg;
							else
							{
								auto fieldRead = awst::makeFieldExpression(readStruct, fn, ft, m_loc);
								newStruct->values[fn] = std::move(fieldRead);
							}
						}
						writeValue = std::move(newStruct);
					}
					else
					{
						writeValue = nullptr;
					}
				}
				else
				{
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

				auto stmt = awst::makeExpressionStatement(std::move(writeBack), m_loc);
				m_ctx.pendingStatements.push_back(std::move(stmt));
			}
		}

		// Memory-ref writeback: each memory-ref arg in the call corresponds
		// to a tuple slot after the storage slots. The arg in the caller is
		// (almost always) a simple VarExpression — the local variable that
		// holds the memory ref. Assign the picked tuple slot back to that
		// variable so subsequent reads see the post-call mutation. Skip if
		// the arg isn't a plain VarExpression — those forms (struct field,
		// array elem, expression result) don't have a stable lvalue we can
		// write back to without re-evaluating side-effecting bases.
		size_t memBaseIdx = baseIdx + roots.size();
		for (size_t mi = 0; mi < memoryRefParamIndices.size(); ++mi)
		{
			size_t pi = memoryRefParamIndices[mi];
			auto const* argVar = dynamic_cast<awst::VarExpression const*>(
				call->args[pi].value.get());
			if (!argVar || argVar->name.empty())
				continue;

			auto* memArgType = call->args[pi].value->wtype;
			auto modifiedArg = pickFromTuple(memBaseIdx + mi, memArgType);

			auto target = awst::makeVarExpression(argVar->name, memArgType, m_loc);
			auto writeBack = awst::makeAssignmentExpression(
				std::move(target), std::move(modifiedArg), m_loc);

			auto stmt = awst::makeExpressionStatement(std::move(writeBack), m_loc);
			m_ctx.pendingStatements.push_back(std::move(stmt));
		}

		return origRet;
	}

	return call;
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
			bool isExternal = funcType->kind() == FunctionType::Kind::External
				|| funcType->kind() == FunctionType::Kind::DelegateCall;

			if (isInternal || isExternal)
			{
				// External fn-ptrs: bytes[12] (appId 8 + selector 4)
				static awst::BytesWType s_extFnPtrType(12);
				awst::WType const* ptrWType = isInternal
					? awst::WType::uint64Type()
					: &s_extFnPtrType;

				std::shared_ptr<awst::Expression> ptrExpr;
				if (varDecl->isStateVariable())
				{
					ptrExpr = m_ctx.storageMapper.createStateRead(
						name, ptrWType,
						awst::AppStorageKind::AppGlobal, m_loc);
				}
				else
				{
					auto var = awst::makeVarExpression(name, ptrWType, m_loc);
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
			auto stmt = awst::makeExpressionStatement(awst::makeAssert(
				awst::makeBoolConstant(false, m_loc), m_loc, "uninitialized function pointer"), m_loc);
			m_ctx.pendingStatements.push_back(std::move(stmt));

			auto vc = awst::makeVoidConstant(m_loc);
			return vc;
		}
	}

	if (auto const* funcDef = dynamic_cast<FunctionDefinition const*>(decl))
	{
		auto* retType = returnTypeFrom(funcDef);
		awst::SubroutineTarget target;

		// MRO super dispatch: when the resolved target is registered as a
		// super-target (e.g. `function() x = super.f;` rebound to `f` and
		// then called via `x()` — solc's referencedDeclaration on super.f
		// gives the right MRO target, but a direct InstanceMethodTarget
		// would bypass the f__super_<callerId> stub the SuperCallResolution
		// machinery built). Mirror the MemberAccess path (line ~670) so
		// the Identifier form picks up super stubs for fn-ptr-bound calls.
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

	// `this.x()` where x is a public state variable for a signed integer
	// ≤64 bits: ContractBuilder's auto-generated getter sets its return
	// type to biguint so it can sign-extend the stored bytes. Mirror that
	// here so the InstanceMethodTarget call's wtype matches.
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
						auto ptrExpr = m_ctx.storageMapper.createStateRead(
							varDecl->name(), awst::WType::uint64Type(),
							awst::AppStorageKind::AppGlobal, m_loc);

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

		// Classify the receiver expression to decide if this is a using-for
		// call (prepend receiver) or a direct `L.f(x, ...)` call.
		auto classifyUsingFor = [&]() -> bool {
			auto const* bt = _memberAccess.expression().annotation().type;
			if (!bt) return true;
			// `import "M" as N; N.f(x)` — N is a Module.
			if (bt->category() == Type::Category::Module) return false;
			// `L.f(x)` where L is a library/contract — TypeType referring to a contract.
			if (bt->category() == Type::Category::TypeType) return false;
			return true;
		};

		// Try AST ID lookup
		auto byId = m_ctx.freeFunctionById.find(funcDef->id());
		if (byId != m_ctx.freeFunctionById.end())
		{
			isUsingForCall = classifyUsingFor();
			return buildSubroutineCall(
				awst::SubroutineID{byId->second}, retType, funcDef, isUsingForCall);
		}

		// Try library function map
		if (auto const* contractDef = funcDef->annotation().contract)
		{
			if (contractDef->isLibrary())
			{
				std::string key = contractDef->name() + "." + funcDef->name();
				auto it = m_ctx.libraryFunctionIds.find(key);
				if (it == m_ctx.libraryFunctionIds.end())
				{
					key += "(" + std::to_string(funcDef->parameters().size()) + ")";
					it = m_ctx.libraryFunctionIds.find(key);
				}
				if (it != m_ctx.libraryFunctionIds.end())
				{
					isUsingForCall = classifyUsingFor();
					return buildSubroutineCall(
						awst::SubroutineID{it->second}, retType, funcDef, isUsingForCall);
				}
			}
		}
	}

	// Struct field holding a function pointer: `s.fn(...)` where `fn` is
	// declared as `function(...) returns (...)` in the struct. The
	// InstanceMethodTarget{"fn"} lookup would try to call `fn` on the
	// current contract, which fails. Instead: read the struct field
	// (FieldExpression → ARC4Decode if needed) to get the pointer id and
	// dispatch via FunctionPointerBuilder.
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
				auto* ptrNativeType = eb::FunctionPointerBuilder::mapFunctionType(funcType);
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

	// Generic function pointer call: evaluate the expression to get a pointer ID,
	// then dispatch through the function pointer table. Try this before the
	// function-pointer-cast resolver so `x()()` (nested fn-ptr returning
	// another fn-ptr that is immediately called) dispatches correctly rather
	// than being mis-classified as a cast with no identifier args.
	{
		auto const* exprType = funcExpr.annotation().type;
		auto const* funcType = dynamic_cast<FunctionType const*>(exprType);
		if (funcType
			&& (funcType->kind() == FunctionType::Kind::Internal
				|| funcType->kind() == FunctionType::Kind::External
				|| funcType->kind() == FunctionType::Kind::DelegateCall))
		{
			auto ptrExpr = m_ctx.buildExpr(funcExpr);
			auto* wantedType = eb::FunctionPointerBuilder::mapFunctionType(funcType);
			// Structural match: uint64 vs uint64, or bytes[12] vs bytes[12]
			// (TypeMapper and FunctionPointerBuilder may create distinct
			// BytesWType instances — compare by shape, not pointer).
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
				// Coerce ARC4-encoded fn-ptr (read from an ARC4 array/struct)
				// to the builder's native ptr type (uint64 or bytes[12]).
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
