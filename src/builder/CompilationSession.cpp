#include "builder/CompilationSession.h"

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
	for (auto const& sourceName: _compiler.sourceNames())
		sourceMap.registerCharStream(
			sourceName, &_compiler.charStream(sourceName));
	for (auto const& [alias, sourceName]: _sourceAliases)
		sourceMap.registerCharStream(
			alias, &_compiler.charStream(sourceName));

	profile = std::move(_profile);
	profile.denseOnlyStorage = false;
	profile.singlePageStorage = false;
	analysis = ProgramAnalysis::analyze(_compiler, profile.evmStorageLayout);
	typeMapper.reset();
}

} // namespace puyasol::builder
