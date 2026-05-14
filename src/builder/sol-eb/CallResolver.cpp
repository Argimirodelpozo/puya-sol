/// @file CallResolver.cpp
/// Resolves function call targets from Solidity AST nodes.

#include "builder/sol-eb/CallResolver.h"
#include "builder/sol-types/OverloadSuffix.h"
#include "Logger.h"

namespace puyasol::builder::eb
{

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

	// Check if in a library scope. solc's Scoper populates
	// `annotation().contract` directly with the enclosing
	// ContractDefinition (or nullptr for free functions in a SourceUnit
	// scope) — saves the scope() + dynamic_cast dance.
	if (auto const* contractDef = _funcDef->annotation().contract)
	{
		if (contractDef->isLibrary())
		{
				// Internalized library function? Route to the per-contract
				// internal method copy via InstanceMethodTarget rather than
				// a root SubroutineID.
				auto internalIt = _ctx.internalizedLibFuncNames.find(_funcDef->id());
				if (internalIt != _ctx.internalizedLibFuncNames.end())
				{
					_result.target = awst::InstanceMethodTarget{internalIt->second};
					_result.funcDef = _funcDef;
					return true;
				}

				// Prefer AST ID lookup for precise overload resolution
				auto byId = _ctx.freeFunctionById.find(_funcDef->id());
				if (byId != _ctx.freeFunctionById.end())
				{
					_result.target = awst::SubroutineID{byId->second};
					_result.funcDef = _funcDef;
					return true;
				}

				// Fallback: name-based lookup
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

	// Check if it's a free function
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

	// Try library/free function resolution
	if (tryResolveLibraryOrFree(_ctx, funcDef, result))
		return result;

	// Regular instance methods fall through to old code for now —
	// too many special cases (argument coercion, return type inference, etc.)
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

	// Check if base is a library identifier: Library.method()
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

	// Check using-for pattern: value.method() where method is library/free function
	auto const* refDecl = _memberAccess.annotation().referencedDeclaration;
	if (auto const* funcDef = dynamic_cast<FunctionDefinition const*>(refDecl))
	{
		result.funcDef = funcDef;
		ResolvedCall tempResult;
		tempResult.funcDef = funcDef;
		if (tryResolveLibraryOrFree(_ctx, funcDef, tempResult))
		{
			result.target = tempResult.target;
			// Determine if receiver should be prepended as first arg.
			// `M.L.f(x)` (module-aliased library) and `L.f(x)` (raw library
			// reference) should both NOT prepend a receiver: the base is a
			// type-level reference, not a value. Only true `value.method(...)`
			// using-for calls prepend.
			auto const* bt = _memberAccess.expression().annotation().type;
			bool isTypeLevelBase = bt
				&& (bt->category() == Type::Category::Module
					|| bt->category() == Type::Category::TypeType);
			result.isUsingForCall = !isTypeLevelBase;
			return result;
		}
	}

	// Super call: super.method()
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

	// Regular instance methods / external calls fall through to old code
	return std::nullopt;
}

} // namespace puyasol::builder::eb
