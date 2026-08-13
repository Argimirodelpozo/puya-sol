/// @file CallResolver.cpp
/// Resolves function call targets from Solidity AST nodes.

#include "builder/itxn/CallResolver.h"
#include "builder/sol-types/OverloadSuffix.h"

namespace puyasol::builder::eb
{

CallPlan CallResolver::plan(solidity::frontend::FunctionCall const& _call)
{
	using namespace solidity::frontend;
	CallPlan result;
	result.functionType = dynamic_cast<FunctionType const*>(
		_call.expression().annotation().type);
	result.callee = &_call.expression();
	if (!result.functionType)
		return result;

	auto kind = result.functionType->kind();
	result.transport = (kind == FunctionType::Kind::External
		|| kind == FunctionType::Kind::DelegateCall)
		? CallTransport::External : CallTransport::Internal;

	if (auto const* options = dynamic_cast<FunctionCallOptions const*>(result.callee))
		result.callee = &options->expression();
	if (auto const* tuple = dynamic_cast<TupleExpression const*>(result.callee);
		tuple && tuple->components().size() == 1 && tuple->components()[0])
		result.callee = tuple->components()[0].get();

	if (auto const* member = dynamic_cast<MemberAccess const*>(result.callee))
	{
		result.declaration = dynamic_cast<FunctionDefinition const*>(
			member->annotation().referencedDeclaration);
		if (result.declaration && result.declaration->annotation().contract
			&& result.declaration->annotation().contract->isLibrary())
			result.transport = CallTransport::Internal;

		auto const* receiver = &member->expression();
		if (auto const* conversion = dynamic_cast<FunctionCall const*>(receiver);
			conversion && conversion->annotation().kind.set()
			&& *conversion->annotation().kind == FunctionCallKind::TypeConversion
			&& conversion->arguments().size() == 1)
			receiver = conversion->arguments()[0].get();
		if (auto const* identifier = dynamic_cast<Identifier const*>(receiver);
			identifier && identifier->name() == "this")
		{
			result.isSelfCall = true;
			result.transport = CallTransport::Internal;
		}

		// A function-valued struct/array member is data, not a contract method.
		auto const* baseType = member->expression().annotation().type;
		while (baseType)
		{
			if (dynamic_cast<StructType const*>(baseType))
			{
				result.isFunctionPointer = true;
				result.transport = CallTransport::Internal;
				break;
			}
			if (auto const* array = dynamic_cast<ArrayType const*>(baseType))
			{
				baseType = array->baseType();
				continue;
			}
			break;
		}
	}
	else if (auto const* identifier = dynamic_cast<Identifier const*>(result.callee))
	{
		if (auto const* variable = dynamic_cast<VariableDeclaration const*>(
				identifier->annotation().referencedDeclaration);
			variable && dynamic_cast<FunctionType const*>(variable->type()))
		{
			result.isFunctionPointer = true;
			result.transport = CallTransport::Internal;
		}
	}
	else if (dynamic_cast<IndexAccess const*>(result.callee)
		|| dynamic_cast<FunctionCall const*>(result.callee))
	{
		result.isFunctionPointer = true;
		result.transport = CallTransport::Internal;
	}

	return result;
}

std::string CallResolver::resolveMethodName(
	ContractContext& _ctx,
	solidity::frontend::FunctionDefinition const& _func)
{
	std::string name = _func.name();
	if (_ctx.overloadedNames.count(name))
		appendOverloadSuffix(name, _func);
	return name;
}

bool CallResolver::tryResolveLibraryOrFree(
	ContractContext& _ctx,
	solidity::frontend::FunctionDefinition const* _funcDef,
	ResolvedCall& _result)
{
	if (!_funcDef)
		return false;

	// solc Scoper populates annotation().contract with the enclosing
	// ContractDefinition (nullptr for free functions in SourceUnit scope).
	if (auto const* contractDef = _funcDef->annotation().contract)
	{
		if (contractDef->isLibrary())
		{
				// Internalized library fn → per-contract copy via InstanceMethodTarget.
				auto internalIt = _ctx.internalizedLibFuncNames.find(_funcDef->id());
				if (internalIt != _ctx.internalizedLibFuncNames.end())
				{
					_result.target = awst::InstanceMethodTarget{internalIt->second};
					_result.funcDef = _funcDef;
					return true;
				}

				// Prefer AST ID for precise overload resolution.
				auto byId = _ctx.freeFunctionById.find(_funcDef->id());
				if (byId != _ctx.freeFunctionById.end())
				{
					_result.target = awst::SubroutineID{byId->second};
					_result.funcDef = _funcDef;
					return true;
				}

				// Fallback: name-based lookup.
				std::string key = contractDef->name() + "." + _funcDef->name();
				auto it = _ctx.libraryFunctionIds.find(key);
				if (it == _ctx.libraryFunctionIds.end())
				{
					key += paramCountSuffix(*_funcDef);
					it = _ctx.libraryFunctionIds.find(key);
				}
				if (it != _ctx.libraryFunctionIds.end())
				{
					_result.target = awst::SubroutineID{it->second};
					_result.funcDef = _funcDef;
					return true;
				}
			}
	}

	// Free function?
	if (_funcDef->isFree())
	{
		auto it = _ctx.freeFunctionById.find(_funcDef->id());
		if (it != _ctx.freeFunctionById.end())
		{
			_result.target = awst::SubroutineID{it->second};
			_result.funcDef = _funcDef;
			return true;
		}
	}

	return false;
}

std::optional<ResolvedCall> CallResolver::resolveFromIdentifier(
	ContractContext& _ctx,
	solidity::frontend::Identifier const& _ident,
	std::string const& _resolvedName)
{
	using namespace solidity::frontend;

	auto const* decl = _ident.annotation().referencedDeclaration;
	auto const* funcDef = dynamic_cast<FunctionDefinition const*>(decl);
	if (!funcDef)
		return std::nullopt;

	ResolvedCall result;
	result.funcDef = funcDef;

	if (tryResolveLibraryOrFree(_ctx, funcDef, result))
		return result;

	// Regular instance methods: fall through to caller (too many special cases).
	return std::nullopt;
}

std::optional<ResolvedCall> CallResolver::resolveFromMemberAccess(
	ContractContext& _ctx,
	sol_ast::Context& _scope,
	solidity::frontend::MemberAccess const& _memberAccess,
	std::string const& _resolvedName,
	size_t _argCount)
{
	using namespace solidity::frontend;

	ResolvedCall result;

	auto const& baseExpr = _memberAccess.expression();
	auto const* baseType = baseExpr.annotation().type;

	// Library.method() pattern.
	if (auto const* baseId = dynamic_cast<Identifier const*>(&baseExpr))
	{
		auto const* decl = baseId->annotation().referencedDeclaration;
		if (auto const* contractDef = dynamic_cast<ContractDefinition const*>(decl))
		{
			if (contractDef->isLibrary())
			{
				auto const* refDecl = _memberAccess.annotation().referencedDeclaration;
				if (auto const* fd = dynamic_cast<FunctionDefinition const*>(refDecl))
				{
					result.funcDef = fd;
					if (tryResolveLibraryOrFree(_ctx, fd, result))
						return result;
				}
			}
		}
	}

	// using-for pattern: value.method() where method is a library/free function.
	auto const* refDecl = _memberAccess.annotation().referencedDeclaration;
	if (auto const* funcDef = dynamic_cast<FunctionDefinition const*>(refDecl))
	{
		result.funcDef = funcDef;
		ResolvedCall tempResult;
		tempResult.funcDef = funcDef;
		if (tryResolveLibraryOrFree(_ctx, funcDef, tempResult))
		{
			result.target = tempResult.target;
			// Don't prepend receiver for `M.L.f(x)` / `L.f(x)` (type-level base);
			// only real using-for value calls prepend.
			auto const* bt = _memberAccess.expression().annotation().type;
			bool isTypeLevelBase = bt
				&& (bt->category() == Type::Category::Module
					|| bt->category() == Type::Category::TypeType);
			result.isUsingForCall = !isTypeLevelBase;
			return result;
		}
	}

	// super.method()
	if (baseType)
	{
		auto const* unwrappedBase = baseType;
		if (baseType->category() == Type::Category::TypeType)
		{
			auto const* typeType = dynamic_cast<TypeType const*>(baseType);
			if (typeType) unwrappedBase = typeType->actualType();
		}

		if (unwrappedBase->category() == Type::Category::Contract)
		{
			auto const* contractType = dynamic_cast<ContractType const*>(unwrappedBase);
			if (contractType && contractType->isSuper())
			{
				if (auto const* fd = dynamic_cast<FunctionDefinition const*>(refDecl))
				{
					result.funcDef = fd;
					result.isSuperCall = true;
					if (auto superName = _scope.findSuperTarget(fd->id()); !superName.empty())
						result.target = awst::InstanceMethodTarget{std::move(superName)};
					else
						result.target = awst::InstanceMethodTarget{_resolvedName};
					return result;
				}
			}
		}
	}

	// Regular instance / external calls: fall through to caller.
	return std::nullopt;
}

} // namespace puyasol::builder::eb
