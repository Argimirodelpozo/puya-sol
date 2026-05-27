#include "builder/FunctionIdRegistry.h"
#include "builder/sol-types/OverloadSuffix.h"
#include "builder/sol-eb/FunctionPointerBuilder.h"
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
	// First pass: register all library and free function IDs (before translating any bodies)
	for (auto const& sourceName: _compiler.sourceNames())
	{
		auto const& sourceUnit = _compiler.ast(sourceName);

		// Library functions — solc's filteredNodes<T> avoids the dynamic_cast loop.
		for (auto const* contract: solidity::frontend::ASTNode::filteredNodes<
			solidity::frontend::ContractDefinition>(sourceUnit.nodes()))
		{
			if (!contract->isLibrary())
				continue;

			std::string libraryName = contract->name();

			// Detect overloaded function names and name+paramcount collisions
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

			// Track sequence numbers for same-name-same-paramcount overloads
			std::unordered_map<std::string, int> nameParamSeq;

			for (auto const* func: contract->definedFunctions())
			{
				if (!func->isImplemented())
					continue;

				// Functions with function-pointer parameters (internal OR external):
				// keep registration so call sites can resolve them, but the actual
				// translation in translateLibraryFunctions will route them through
				// per-contract internalization (see m_internalizableLibFuncs).

				std::string baseName = libraryName + "." + func->name();
				std::string qualifiedName = baseName;
				std::string subroutineId = _sourceFile + "." + baseName;
				// Disambiguate overloaded functions by parameter count
				if (nameCount[baseName] > 1)
				{
					std::string paramKey = baseName + paramCountSuffix(*func);
					qualifiedName = paramKey;
					subroutineId = _sourceFile + "." + paramKey;
					// Further disambiguate if same name AND same param count
					if (nameParamCount[paramKey] > 1)
					{
						int seq = nameParamSeq[paramKey]++;
						qualifiedName += "_" + std::to_string(seq);
						subroutineId += "_" + std::to_string(seq);
					}
				}
				m_libraryFunctionIds[qualifiedName] = subroutineId;
				// Also store by AST ID for precise overload resolution
				m_freeFunctionById[func->id()] = subroutineId;
				Logger::instance().debug("[REG] lib func id=" + std::to_string(func->id()) + " name=" + qualifiedName + " => " + subroutineId);
			}
		}

		// Free (file-level) functions — same filteredNodes treatment.
		for (auto const* func: solidity::frontend::ASTNode::filteredNodes<
			solidity::frontend::FunctionDefinition>(sourceUnit.nodes()))
		{
			if (!func->isImplemented() || !func->isFree())
				continue;

			std::string qualifiedName = func->name();
			std::string subroutineId = _sourceFile + "." + qualifiedName;
			// Disambiguate free functions with the same name (e.g. UD60x18.powu vs SD59x18.powu)
			// by appending the AST ID when the name is already registered by a different function
			auto existingIt = m_freeFunctionById.find(func->id());
			if (existingIt == m_freeFunctionById.end())
			{
				// Check if another function already uses this name
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
			// Also store by AST ID for operator overload resolution
			m_freeFunctionById[func->id()] = subroutineId;
			Logger::instance().debug("[REG] free func id=" + std::to_string(func->id()) + " name=" + qualifiedName + " => " + subroutineId);
		}
	}
}

void presetDispatchCref(
	solidity::frontend::CompilerStack& _compiler,
	std::string const& _sourceFile)
{
	// Pre-set the function pointer dispatch cref to the first deployable
	// contract so that library subroutines can construct SubroutineIDs
	// for dispatch calls. Library bodies are translated before contracts,
	// but the dispatch subroutines live in the contract scope.
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
