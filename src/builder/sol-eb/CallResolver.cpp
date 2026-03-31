/// @file CallResolver.cpp
/// Resolves function call targets from Solidity AST nodes.

#include "builder/sol-eb/CallResolver.h"
#include "Logger.h"

namespace puyasol::builder::eb
{

std::string CallResolver::resolveMethodName(
	BuilderContext& _ctx,
	solidity::frontend::FunctionDefinition const& _func)
{
	std::string name = _func.name();
	if (_ctx.overloadedNames.count(name))
	{
		name += "(";
		bool first = true;
		for (auto const& p: _func.parameters())
		{
			if (!first) name += ",";
			auto const* solType = p->type();
			if (dynamic_cast<solidity::frontend::BoolType const*>(solType))
				name += "b";
			else if (auto const* intType = dynamic_cast<solidity::frontend::IntegerType const*>(solType))
				name += (intType->isSigned() ? "i" : "u") + std::to_string(intType->numBits());
			else if (dynamic_cast<solidity::frontend::AddressType const*>(solType))
				name += "addr";
			else if (auto const* fixedBytes = dynamic_cast<solidity::frontend::FixedBytesType const*>(solType))
				name += "b" + std::to_string(fixedBytes->numBytes());
			else
				name += std::to_string(p->id());
			first = false;
		}
		name += ")";
	}
	return name;
}

bool CallResolver::tryResolveLibraryOrFree(
	BuilderContext& _ctx,
	solidity::frontend::FunctionDefinition const* _funcDef,
	ResolvedCall& _result)
{
	if (!_funcDef)
		return false;

	// Check if in a library scope
	if (auto const* scope = _funcDef->scope())
	{
		if (auto const* contractDef = dynamic_cast<solidity::frontend::ContractDefinition const*>(scope))
		{
			if (contractDef->isLibrary())
			{
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
					key += "(" + std::to_string(_funcDef->parameters().size()) + ")";
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
	BuilderContext& _ctx,
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
	BuilderContext& _ctx,
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
			// Determine if receiver should be prepended as first arg
			auto const* bt = _memberAccess.expression().annotation().type;
			bool isModuleCall = bt && bt->category() == Type::Category::Module;
			result.isUsingForCall = !isModuleCall;
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
					auto it = _ctx.superTargetNames.find(fd->id());
					if (it != _ctx.superTargetNames.end())
						result.target = awst::InstanceMethodTarget{it->second};
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
