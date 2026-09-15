#include "builder/context/CompilationSession.h"

namespace puyasol::builder
{

void CompilationSession::begin(
	solidity::frontend::CompilerStack& _compiler,
	std::map<std::string, std::string> const& _sourceAliases,
	TargetProfile _profile)
{
	awst::NameGen::resetAll();
	functionPointers.reset();
	artifacts.clear();
	storagePlans.clear();
	sourceMap.clear();
	std::map<std::string, std::string> readableFiles;
	for (auto const& [alias, sourceName]: _sourceAliases)
		readableFiles.emplace(sourceName, alias);
	for (auto const& sourceName: _compiler.sourceNames())
		sourceMap.registerCharStream(
			sourceName, &_compiler.charStream(sourceName), readableFiles[sourceName]);
	for (auto const& [alias, sourceName]: _sourceAliases)
		sourceMap.registerCharStream(
			alias, &_compiler.charStream(sourceName), alias);

	profile = std::move(_profile);
	analysis = ProgramAnalysis::analyze(_compiler, profile.evmStorageLayout);
	if (profile.proxyAdaptation)
	{
		analysis.proxy = proxies::ProxyFacts::analyze(analysis.contracts, sourceMap);
		// Native lifecycle hooks are additional roots, absent from solc's EVM
		// entry points. Close their existing solc declaration-reference edges.
		for (auto const& [contractId, hook]: analysis.proxy.authorizationHooks)
		{
			auto& reachable = analysis.reachableCallablesByContract[contractId];
			reachable.insert(hook->id());
			analysis.closeCallableReferences(reachable);
			analysis.reachableCallableIds.insert(reachable.begin(), reachable.end());
			analysis.internallyCalledFunctions[contractId].insert(hook->id());
		}
	}
	if (profile.scratchMemoryModel)
		analysis.memorySharing = analyzeMemorySharing(analysis);
	typeMapper.reset();
}

} // namespace puyasol::builder
