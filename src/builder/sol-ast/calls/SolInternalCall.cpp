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
	if (_funcDef->returnParameters().size() == 1)
	{
		// Storage reference return with .slot assembly → biguint (slot number)
		if (_funcDef->returnParameters()[0]->referenceLocation() == VariableDeclaration::Location::Storage
			&& _funcDef->isImplemented()
			&& std::any_of(_funcDef->body().statements().begin(), _funcDef->body().statements().end(),
				[](auto const& s) { return dynamic_cast<InlineAssembly const*>(s.get()); }))
			return awst::WType::biguintType();
		return m_ctx.typeMapper.map(_funcDef->returnParameters()[0]->type());
	}

	std::vector<awst::WType const*> retTypes;
	for (auto const& param: _funcDef->returnParameters())
		retTypes.push_back(m_ctx.typeMapper.map(param->type()));
	return m_ctx.typeMapper.createType<awst::WTuple>(std::move(retTypes), std::nullopt);
}

std::shared_ptr<awst::Expression> SolInternalCall::buildSubroutineCall(
	awst::SubroutineTarget _target,
	awst::WType const* _returnType,
	FunctionDefinition const* _funcDef,
	bool _isUsingForCall)
{
	// Function-type parameters: only internal function pointers passed as params
	// are supported. External/other function-type params are skipped.
	if (_funcDef)
	{
		bool hasFunctionParam = false;
		bool allInternal = true;
		for (auto const& p: _funcDef->parameters())
		{
			if (auto const* ft = dynamic_cast<FunctionType const*>(p->type()))
			{
				hasFunctionParam = true;
				if (ft->kind() != FunctionType::Kind::Internal)
					allInternal = false;
			}
		}
		if (hasFunctionParam && !allInternal)
		{
			Logger::instance().warning(
				"skipping call to '" + _funcDef->name()
				+ "' (has non-internal function-type parameter)", m_loc);
			auto noop = std::make_shared<awst::IntrinsicCall>();
			noop->sourceLocation = m_loc;
			noop->wtype = awst::WType::voidType();
			noop->opCode = "log";
			auto msg = std::make_shared<awst::BytesConstant>();
			msg->sourceLocation = m_loc;
			msg->wtype = awst::WType::bytesType();
			msg->value = {};
			noop->stackArgs.push_back(std::move(msg));
			return noop;
		}
	}

	auto call = std::make_shared<awst::SubroutineCallExpression>();
	call->sourceLocation = m_loc;
	call->wtype = _returnType;
	call->target = std::move(_target);

	// Collect parameter types for coercion
	std::vector<awst::WType const*> paramTypes;
	if (_funcDef)
	{
		for (auto const& param: _funcDef->parameters())
			paramTypes.push_back(m_ctx.typeMapper.map(param->type()));
	}

	// For using-for calls, prepend receiver as first arg
	if (_isUsingForCall)
	{
		auto const& funcExpr = funcExpression();
		if (auto const* memberAccess = dynamic_cast<MemberAccess const*>(&funcExpr))
		{
			awst::CallArg ca;
			ca.value = buildExpr(memberAccess->expression());
			if (!paramTypes.empty())
				ca.value = builder::TypeCoercion::implicitNumericCast(
					std::move(ca.value), paramTypes[0], m_loc);
			call->args.push_back(std::move(ca));
		}
	}

	// Build arguments with type coercion
	auto const sortedArgs = m_call.sortedArguments();
	for (size_t i = 0; i < sortedArgs.size(); ++i)
	{
		awst::CallArg ca;
		ca.value = buildExpr(*sortedArgs[i]);
		size_t paramIdx = _isUsingForCall ? (i + 1) : i;
		if (paramIdx < paramTypes.size())
		{
			ca.value = builder::TypeCoercion::implicitNumericCast(
				std::move(ca.value), paramTypes[paramIdx], m_loc);
		}
		call->args.push_back(std::move(ca));
	}

	// Storage write-back for using-for calls on box-backed storage
	if (_isUsingForCall && _funcDef && !call->args.empty()
		&& _funcDef->stateMutability() != StateMutability::View
		&& _funcDef->stateMutability() != StateMutability::Pure
		&& !_funcDef->parameters().empty()
		&& _funcDef->parameters()[0]->referenceLocation()
			== VariableDeclaration::Location::Storage)
	{
		auto const* receiverExpr = call->args[0].value.get();
		std::shared_ptr<awst::BoxValueExpression> rootBox;
		std::vector<std::string> fieldPath;

		std::function<void(awst::Expression const*)> traceToBox;
		traceToBox = [&](awst::Expression const* e) {
			if (auto const* field = dynamic_cast<awst::FieldExpression const*>(e)) {
				fieldPath.push_back(field->name);
				traceToBox(field->base.get());
			} else if (auto const* sg = dynamic_cast<awst::StateGet const*>(e)) {
				traceToBox(sg->field.get());
			} else if (auto const* box = dynamic_cast<awst::BoxValueExpression const*>(e)) {
				auto b = std::make_shared<awst::BoxValueExpression>();
				b->sourceLocation = box->sourceLocation;
				b->wtype = box->wtype;
				b->key = box->key;
				b->existsAssertionMessage = std::nullopt;
				rootBox = b;
			}
		};
		traceToBox(receiverExpr);

		if (rootBox && !fieldPath.empty())
		{
			auto* origRetType = call->wtype;
			auto* storageArgType = call->args[0].value->wtype;

			auto* tupleType = m_ctx.typeMapper.createType<awst::WTuple>(
				std::vector<awst::WType const*>{origRetType, storageArgType});
			call->wtype = tupleType;

			static int storageWriteBackCounter = 0;
			std::string tempName = "__storage_wb_" + std::to_string(storageWriteBackCounter++);

			auto tempVar = std::make_shared<awst::VarExpression>();
			tempVar->sourceLocation = m_loc;
			tempVar->wtype = tupleType;
			tempVar->name = tempName;

			auto assignTemp = std::make_shared<awst::AssignmentStatement>();
			assignTemp->sourceLocation = m_loc;
			assignTemp->target = tempVar;
			assignTemp->value = std::shared_ptr<awst::Expression>(call);
			m_ctx.prePendingStatements.push_back(std::move(assignTemp));

			auto origRet = std::make_shared<awst::TupleItemExpression>();
			origRet->sourceLocation = m_loc;
			origRet->wtype = origRetType;
			origRet->base = tempVar;
			origRet->index = 0;

			auto modifiedArg = std::make_shared<awst::TupleItemExpression>();
			modifiedArg->sourceLocation = m_loc;
			modifiedArg->wtype = storageArgType;
			modifiedArg->base = tempVar;
			modifiedArg->index = 1;

			std::reverse(fieldPath.begin(), fieldPath.end());

			auto readStruct = std::make_shared<awst::StateGet>();
			readStruct->sourceLocation = m_loc;
			readStruct->wtype = rootBox->wtype;
			readStruct->field = rootBox;
			readStruct->defaultValue = builder::StorageMapper::makeDefaultValue(rootBox->wtype, m_loc);

			auto const* structType = dynamic_cast<awst::ARC4Struct const*>(rootBox->wtype);
			if (structType && fieldPath.size() == 1)
			{
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

				auto writeBack = std::make_shared<awst::AssignmentExpression>();
				writeBack->sourceLocation = m_loc;
				writeBack->wtype = rootBox->wtype;
				writeBack->target = rootBox;
				writeBack->value = std::move(newStruct);

				auto stmt = std::make_shared<awst::ExpressionStatement>();
				stmt->sourceLocation = m_loc;
				stmt->expr = std::move(writeBack);
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
			if (funcType->kind() == FunctionType::Kind::Internal)
			{
				std::shared_ptr<awst::Expression> ptrExpr;

				if (varDecl->isStateVariable())
				{
					// State variable: read function ID from global state
					ptrExpr = m_ctx.storageMapper.createStateRead(
						name, awst::WType::uint64Type(),
						awst::AppStorageKind::AppGlobal, m_loc);
				}
				else
				{
					// Local/param: use variable directly
					ptrExpr = std::make_shared<awst::VarExpression>();
					std::static_pointer_cast<awst::VarExpression>(ptrExpr)->sourceLocation = m_loc;
					std::static_pointer_cast<awst::VarExpression>(ptrExpr)->wtype = awst::WType::uint64Type();
					std::static_pointer_cast<awst::VarExpression>(ptrExpr)->name = name;
				}

				std::vector<std::shared_ptr<awst::Expression>> args;
				for (auto const& arg : m_call.arguments())
					args.push_back(m_ctx.buildExpr(*arg));

				auto result = eb::FunctionPointerBuilder::buildFunctionPointerCall(
					m_ctx, std::move(ptrExpr), funcType, std::move(args), m_loc);
				if (result)
					return result;
			}

			// Fallback for external / unsupported:
			// emit assert(false) to revert (matches EVM behavior for uninitialized pointers)
			Logger::instance().warning(
				"call to function pointer '" + name + "' (state var / unsupported), emitting assert(false)", m_loc);
			auto assertExpr = std::make_shared<awst::AssertExpression>();
			assertExpr->sourceLocation = m_loc;
			assertExpr->wtype = awst::WType::voidType();
			auto falseLit = std::make_shared<awst::BoolConstant>();
			falseLit->sourceLocation = m_loc;
			falseLit->wtype = awst::WType::boolType();
			falseLit->value = false;
			assertExpr->condition = std::move(falseLit);
			assertExpr->errorMessage = "uninitialized function pointer";
			auto stmt = std::make_shared<awst::ExpressionStatement>();
			stmt->sourceLocation = m_loc;
			stmt->expr = std::move(assertExpr);
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

		// Try AST ID lookup
		auto byId = m_ctx.freeFunctionById.find(funcDef->id());
		if (byId != m_ctx.freeFunctionById.end())
		{
			auto const* bt = _memberAccess.expression().annotation().type;
			bool isModCall = bt && bt->category() == Type::Category::Module;
			isUsingForCall = !isModCall;
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
						auto const* bt = _memberAccess.expression().annotation().type;
						bool isModCall = bt && bt->category() == Type::Category::Module;
						isUsingForCall = !isModCall;
						return buildSubroutineCall(
							awst::SubroutineID{it->second}, retType, funcDef, isUsingForCall);
					}
				}
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

	if (auto const* innerCall = dynamic_cast<FunctionCall const*>(&funcExpr))
		return resolveFunctionPointerCast(*innerCall);

	// Generic function pointer call: evaluate the expression to get a pointer ID,
	// then dispatch through the function pointer table.
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

	// Fallback: unresolvable call
	Logger::instance().warning("could not resolve function call target", m_loc);
	auto* retType = m_ctx.typeMapper.map(m_call.annotation().type);
	return buildSubroutineCall(
		awst::InstanceMethodTarget{"unknown"}, retType, nullptr, false);
}

} // namespace puyasol::builder::sol_ast
