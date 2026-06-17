#include "builder/FunctionIdRegistry.h"
#include "builder/sol-types/OverloadSuffix.h"
#include "builder/itxn/FunctionPointerBuilder.h"
#include "Logger.h"

#include <libsolidity/ast/AST.h>

#include <unordered_map>

namespace puyasol::builder
{

void registerFunctionIds(
	solidity::frontend::CompilerStack& _compiler,
	std::string const& _sourceFile,
	LibraryFunctionIdMap& m_libraryFunctionIds,
	FreeFunctionIdMap& m_freeFunctionById)
{
	for (auto const& sourceName: _compiler.sourceNames())
	{
		auto const& sourceUnit = _compiler.ast(sourceName);

		for (auto const* contract: solidity::frontend::ASTNode::filteredNodes<
			solidity::frontend::ContractDefinition>(sourceUnit.nodes()))
		{
			if (!contract->isLibrary())
				continue;

			std::string libraryName = contract->name();

			// Detect overloaded function names and name+paramcount collisions.
			std::unordered_map<std::string, int> nameCount;
			std::unordered_map<std::string, int> nameParamCount;
			for (auto const* func: contract->definedFunctions())
			{
				if (!func->isImplemented())
					continue;
				std::string baseName = libraryName + "." + func->name();
				nameCount[baseName]++;
				nameParamCount[baseName + paramCountSuffix(*func)]++;
			}

			// Track sequence numbers for same-name-same-paramcount overloads.
			std::unordered_map<std::string, int> nameParamSeq;

			for (auto const* func: contract->definedFunctions())
			{
				if (!func->isImplemented())
					continue;

				std::string baseName = libraryName + "." + func->name();
				std::string qualifiedName = baseName;
				std::string subroutineId = _sourceFile + "." + baseName;
				// Disambiguate by parameter count.
				if (nameCount[baseName] > 1)
				{
					std::string paramKey = baseName + paramCountSuffix(*func);
					qualifiedName = paramKey;
					subroutineId = _sourceFile + "." + paramKey;
					// Further disambiguate: same name + same param count.
					if (nameParamCount[paramKey] > 1)
					{
						int seq = nameParamSeq[paramKey]++;
						qualifiedName += "_" + std::to_string(seq);
						subroutineId += "_" + std::to_string(seq);
					}
				}
				m_libraryFunctionIds[qualifiedName] = subroutineId;
				// Also index by AST ID for precise overload resolution.
				m_freeFunctionById[func->id()] = subroutineId;
				Logger::instance().debug("[REG] lib func id=" + std::to_string(func->id()) + " name=" + qualifiedName + " => " + subroutineId);
			}
		}

		for (auto const* func: solidity::frontend::ASTNode::filteredNodes<
			solidity::frontend::FunctionDefinition>(sourceUnit.nodes()))
		{
			if (!func->isImplemented() || !func->isFree())
				continue;

			std::string qualifiedName = func->name();
			std::string subroutineId = _sourceFile + "." + qualifiedName;
			// Disambiguate same-name free functions (e.g. UD60x18.powu vs SD59x18.powu)
			// by appending the AST ID when the name is already registered by another function.
			auto existingIt = m_freeFunctionById.find(func->id());
			if (existingIt == m_freeFunctionById.end())
			{
				for (auto const& [otherId, otherSid]: m_freeFunctionById)
				{
					if (otherSid == subroutineId && otherId != func->id())
					{
						subroutineId += "_" + std::to_string(func->id());
						break;
					}
				}
			}
			m_libraryFunctionIds[qualifiedName] = subroutineId;
			m_freeFunctionById[func->id()] = subroutineId;
			Logger::instance().debug("[REG] free func id=" + std::to_string(func->id()) + " name=" + qualifiedName + " => " + subroutineId);
		}
	}
}

void presetDispatchCref(
	solidity::frontend::CompilerStack& _compiler,
	std::string const& _sourceFile)
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
				eb::FunctionPointerBuilder::setCurrentCref(_sourceFile + "." + c->name());
				return;
			}
		}
	}
}

} // namespace puyasol::builder
