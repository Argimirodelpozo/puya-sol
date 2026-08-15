#include "builder/FunctionIdRegistry.h"
#include "builder/itxn/FunctionPointerBuilder.h"
#include "Logger.h"

#include <libsolidity/ast/AST.h>

namespace puyasol::builder
{

void registerFunctionIds(
	solidity::frontend::CompilerStack& _compiler,
	FunctionSymbolTable& _functionSymbols)
{
	_functionSymbols.clear();
	for (auto const& sourceName: _compiler.sourceNames())
	{
		auto const& sourceUnit = _compiler.ast(sourceName);

		for (auto const* contract: solidity::frontend::ASTNode::filteredNodes<
			solidity::frontend::ContractDefinition>(sourceUnit.nodes()))
		{
			for (auto const* function: contract->definedFunctions())
			{
				if (!function->isImplemented() || function->isConstructor())
					continue;
				bool const rootSubroutine = contract->isLibrary();
				bool const internalMethod =
					function->visibility() == solidity::frontend::Visibility::Internal
					|| function->visibility() == solidity::frontend::Visibility::Private;
				if (!rootSubroutine && !internalMethod)
					continue;
				auto const& subroutineId = _functionSymbols.registerDeclaration(
					function->id(), rootSubroutine);
				Logger::instance().debug(
					"[REG] Solidity function declaration=" +
					std::to_string(function->id()) + " => " + subroutineId);
			}
		}

		for (auto const* function: solidity::frontend::ASTNode::filteredNodes<
			solidity::frontend::FunctionDefinition>(sourceUnit.nodes()))
		{
			if (!function->isImplemented() || !function->isFree())
				continue;
			auto const& subroutineId = _functionSymbols.registerDeclaration(
				function->id(), /*_rootSubroutine=*/true);
			Logger::instance().debug(
				"[REG] free function declaration=" +
				std::to_string(function->id()) + " => " + subroutineId);
		}
	}
}

void presetDispatchCref(
	solidity::frontend::CompilerStack& _compiler,
	std::string const& _sourceFile,
	eb::FunctionPointerRegistry& _functionPointers)
{
	// Set fn-ptr dispatch cref to the first deployable contract so library
	// subroutines can build SubroutineIDs (libs translated before contracts).
	for (auto const& sourceName: _compiler.sourceNames())
	{
		auto const& su = _compiler.ast(sourceName);
		for (auto const* c: solidity::frontend::ASTNode::filteredNodes<
			solidity::frontend::ContractDefinition>(su.nodes()))
		{
			if (!c->isLibrary() && !c->abstract() && !c->isInterface())
			{
				_functionPointers.currentCref = _sourceFile + "." + c->name();
				return;
			}
		}
	}
}

} // namespace puyasol::builder
