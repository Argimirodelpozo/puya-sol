/// @file SolInternalCall.cpp
/// Internal function call resolution and SubroutineCallExpression building.
/// Migrated from FunctionCallBuilder.cpp lines 3324-4390.

#include "builder/sol-ast/calls/SolInternalCall.h"
#include "builder/sol-eb/CallResolver.h"
#include "builder/sol-eb/FunctionPointerBuilder.h"
#include "builder/sol-types/TypeMapper.h"
#include "builder/sol-types/TypeCoercion.h"
#include "builder/storage/StorageMapper.h"
#include "Logger.h"

#include <libsolidity/ast/AST.h>

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
	// extension works; we mirror that here so call-site wtypes match.
	auto mapReturnType = [&](solidity::frontend::Type const* solType) -> awst::WType const* {
		auto* mapped = m_ctx.typeMapper.map(solType);
		auto const* t = solType;
		if (auto const* udvt = dynamic_cast<UserDefinedValueType const*>(t))
			t = &udvt->underlyingType();
		auto const* intType = dynamic_cast<IntegerType const*>(t);
		if (!intType)
			if (auto const* enumType = dynamic_cast<EnumType const*>(t))
				intType = dynamic_cast<IntegerType const*>(enumType->encodingType());
		if (intType && intType->isSigned() && intType->numBits() <= 64)
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

	auto call = std::make_shared<awst::SubroutineCallExpression>();
	call->sourceLocation = m_loc;
	call->wtype = _returnType;
	call->target = std::move(_target);

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

	// Storage write-back for library calls whose first parameter is a
	// storage reference. AWSTBuilder augments non-private, non-pure/view
	// library functions to thread the modified storage arg back through
	// the return value as `WTuple(R, T)` (or just `T` when R is void).
	//
	// Only library functions get this augmentation — contract methods and
	// free functions are NOT augmented. So the call-site unpack must be
	// scoped to library callees, otherwise we'd build a tuple return for
	// a callee that actually returns a scalar.
	//
	// Two receiver shapes are supported:
	//  1. Box-backed state (StateGet → BoxValueExpression), optionally with
	//     a single-level FieldExpression (`x.field.method(...)`).
	//  2. Direct AppStateExpression (`x.method(...)` where x is a non-box
	//     state variable — the common case for small struct state vars).
	bool calleeIsLibrary = false;
	bool calleeIsPrivate = false;
	if (_funcDef)
	{
		calleeIsPrivate = _funcDef->visibility() == Visibility::Private;
		if (auto const* scope = _funcDef->scope())
			if (auto const* contractDef = dynamic_cast<ContractDefinition const*>(scope))
				calleeIsLibrary = contractDef->isLibrary();
	}
	if (_funcDef && !call->args.empty()
		&& calleeIsLibrary && !calleeIsPrivate
		&& _funcDef->stateMutability() != StateMutability::View
		&& _funcDef->stateMutability() != StateMutability::Pure
		&& !_funcDef->parameters().empty()
		&& _funcDef->parameters()[0]->referenceLocation()
			== VariableDeclaration::Location::Storage)
	{
		auto const* receiverExpr = call->args[0].value.get();
		std::shared_ptr<awst::BoxValueExpression> rootBox;
		std::shared_ptr<awst::AppStateExpression> rootAppState;
		std::vector<std::string> fieldPath;

		std::function<void(awst::Expression const*)> traceToRoot;
		traceToRoot = [&](awst::Expression const* e) {
			if (auto const* field = dynamic_cast<awst::FieldExpression const*>(e)) {
				fieldPath.push_back(field->name);
				traceToRoot(field->base.get());
			} else if (auto const* sg = dynamic_cast<awst::StateGet const*>(e)) {
				traceToRoot(sg->field.get());
			} else if (auto const* box = dynamic_cast<awst::BoxValueExpression const*>(e)) {
				auto b = std::make_shared<awst::BoxValueExpression>();
				b->sourceLocation = box->sourceLocation;
				b->wtype = box->wtype;
				b->key = box->key;
				b->existsAssertionMessage = std::nullopt;
				rootBox = b;
			} else if (auto const* app = dynamic_cast<awst::AppStateExpression const*>(e)) {
				auto a = std::make_shared<awst::AppStateExpression>();
				a->sourceLocation = app->sourceLocation;
				a->wtype = app->wtype;
				a->key = app->key;
				a->existsAssertionMessage = std::nullopt;
				rootAppState = a;
			}
		};
		traceToRoot(receiverExpr);

		bool hasRoot = (rootBox != nullptr) || (rootAppState != nullptr);
		if (hasRoot)
		{
			auto* origRetType = call->wtype;
			auto* storageArgType = call->args[0].value->wtype;
			auto* rootType = rootBox ? rootBox->wtype : rootAppState->wtype;

			// AWSTBuilder augments library function returns:
			//   (R, T) when the solidity function has a return type R
			//   (T,)  when the solidity function returns void — in which case
			//         the augmented single-element return is just storage_type T
			//         itself (not wrapped in a tuple).
			bool voidReturn = (origRetType == awst::WType::voidType());

			static int storageWriteBackCounter = 0;
			std::string tempName = "__storage_wb_" + std::to_string(storageWriteBackCounter++);

			std::shared_ptr<awst::VarExpression> tempVar;
			std::shared_ptr<awst::Expression> origRet;
			std::shared_ptr<awst::Expression> modifiedArg;

			if (voidReturn)
			{
				// Augmented signature: () → storage_type. No tuple needed.
				call->wtype = storageArgType;

				tempVar = std::make_shared<awst::VarExpression>();
				tempVar->sourceLocation = m_loc;
				tempVar->wtype = storageArgType;
				tempVar->name = tempName;

				auto assignTemp = awst::makeAssignmentStatement(tempVar, std::shared_ptr<awst::Expression>(call), m_loc);
				m_ctx.prePendingStatements.push_back(std::move(assignTemp));

				modifiedArg = tempVar;
				origRet = std::make_shared<awst::VoidConstant>();
				origRet->sourceLocation = m_loc;
				origRet->wtype = awst::WType::voidType();
			}
			else
			{
				auto* tupleType = m_ctx.typeMapper.createType<awst::WTuple>(
					std::vector<awst::WType const*>{origRetType, storageArgType});
				call->wtype = tupleType;

				tempVar = std::make_shared<awst::VarExpression>();
				tempVar->sourceLocation = m_loc;
				tempVar->wtype = tupleType;
				tempVar->name = tempName;

				auto assignTemp = awst::makeAssignmentStatement(tempVar, std::shared_ptr<awst::Expression>(call), m_loc);
				m_ctx.prePendingStatements.push_back(std::move(assignTemp));

				auto origTup = std::make_shared<awst::TupleItemExpression>();
				origTup->sourceLocation = m_loc;
				origTup->wtype = origRetType;
				origTup->base = tempVar;
				origTup->index = 0;
				origRet = std::move(origTup);

				auto modTup = std::make_shared<awst::TupleItemExpression>();
				modTup->sourceLocation = m_loc;
				modTup->wtype = storageArgType;
				modTup->base = tempVar;
				modTup->index = 1;
				modifiedArg = std::move(modTup);
			}

			std::reverse(fieldPath.begin(), fieldPath.end());

			// Value to write back to the root state. When fieldPath is empty
			// (receiver IS the root struct), it's the modifiedArg directly.
			// When fieldPath has a single field (receiver is `root.field`),
			// we have to read the other fields of root and build a NewStruct
			// so the final assignment replaces only the touched field.
			std::shared_ptr<awst::Expression> writeValue = modifiedArg;
			if (!fieldPath.empty())
			{
				if (fieldPath.size() == 1)
				{
					auto const* structType =
						dynamic_cast<awst::ARC4Struct const*>(rootType);
					if (structType)
					{
						std::shared_ptr<awst::Expression> readStruct;
						if (rootBox)
						{
							auto sg = std::make_shared<awst::StateGet>();
							sg->sourceLocation = m_loc;
							sg->wtype = rootType;
							sg->field = rootBox;
							sg->defaultValue =
								builder::StorageMapper::makeDefaultValue(rootType, m_loc);
							readStruct = std::move(sg);
						}
						else
						{
							readStruct = rootAppState;
						}

						auto newStruct = std::make_shared<awst::NewStruct>();
						newStruct->sourceLocation = m_loc;
						newStruct->wtype = structType;
						for (auto const& [fn, ft]: structType->fields())
						{
							if (fn == fieldPath[0])
								newStruct->values[fn] = modifiedArg;
							else
							{
								auto fieldRead = std::make_shared<awst::FieldExpression>();
								fieldRead->sourceLocation = m_loc;
								fieldRead->wtype = ft;
								fieldRead->base = readStruct;
								fieldRead->name = fn;
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
					rootBox ? std::static_pointer_cast<awst::Expression>(rootBox)
							: std::static_pointer_cast<awst::Expression>(rootAppState);

				auto writeBack = std::make_shared<awst::AssignmentExpression>();
				writeBack->sourceLocation = m_loc;
				writeBack->wtype = rootType;
				writeBack->target = std::move(writeTarget);
				writeBack->value = std::move(writeValue);

				auto stmt = awst::makeExpressionStatement(std::move(writeBack), m_loc);
				m_ctx.pendingStatements.push_back(std::move(stmt));
			}

			return origRet;
		}
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
		auto it = m_ctx.funcPtrTargets.find(varDecl->id());
		if (it != m_ctx.funcPtrTargets.end() && it->second)
		{
			decl = it->second;
			Logger::instance().debug("resolved function pointer '" + name + "' to '" + it->second->name() + "'");
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

			auto vc = std::make_shared<awst::VoidConstant>();
			vc->sourceLocation = m_loc;
			vc->wtype = awst::WType::voidType();
			return vc;
		}
	}

	if (auto const* funcDef = dynamic_cast<FunctionDefinition const*>(decl))
	{
		auto* retType = returnTypeFrom(funcDef);
		awst::SubroutineTarget target;

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

	// Try CallResolver first (handles library, free, using-for, super)
	auto resolved = eb::CallResolver::resolveFromMemberAccess(
		m_ctx, _memberAccess,
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
				auto superIt = m_ctx.superTargetNames.find(funcDef->id());
				if (superIt != m_ctx.superTargetNames.end())
				{
					auto target = awst::InstanceMethodTarget{superIt->second};
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
		if (auto const* scope = funcDef->scope())
		{
			if (auto const* contractDef = dynamic_cast<ContractDefinition const*>(scope))
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
					auto field = std::make_shared<awst::FieldExpression>();
					field->sourceLocation = m_loc;
					field->base = std::move(baseExpr);
					field->name = _memberAccess.memberName();
					field->wtype = arc4FieldType ? arc4FieldType : ptrNativeType;
					if (arc4FieldType && arc4FieldType != ptrNativeType)
					{
						auto decode = std::make_shared<awst::ARC4Decode>();
						decode->sourceLocation = m_loc;
						decode->wtype = ptrNativeType;
						decode->value = std::move(field);
						ptrExpr = std::move(decode);
					}
					else
						ptrExpr = std::move(field);
				}
				else
				{
					auto field = std::make_shared<awst::FieldExpression>();
					field->sourceLocation = m_loc;
					field->base = std::move(baseExpr);
					field->name = _memberAccess.memberName();
					field->wtype = ptrNativeType;
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

	Logger::instance().warning("could not resolve function call target", m_loc);
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
		if (funcType && funcType->kind() == FunctionType::Kind::Internal)
		{
			auto ptrExpr = m_ctx.buildExpr(funcExpr);
			if (ptrExpr && ptrExpr->wtype == awst::WType::uint64Type())
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
	Logger::instance().warning("could not resolve function call target", m_loc);
	auto* retType = m_ctx.typeMapper.map(m_call.annotation().type);
	return buildSubroutineCall(
		awst::InstanceMethodTarget{"unknown"}, retType, nullptr, false);
}

} // namespace puyasol::builder::sol_ast
