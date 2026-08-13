#pragma once

#include "builder/ProgramAnalysis.h"
#include "builder/SourceLocConvert.h"
#include "builder/BuildArtifacts.h"
#include "builder/TargetProfile.h"
#include "builder/sol-types/TypeMapper.h"
#include "builder/itxn/FunctionPointerBuilder.h"
#include "awst/NameGen.h"

#include <libsolidity/interface/CompilerStack.h>
#include <liblangutil/EVMVersion.h>

#include <map>
#include <string>

namespace puyasol::builder
{

/// Owns all state whose lifetime is one compiler invocation. The object itself
/// is stable inside AWSTBuilder so services can safely retain references to its
/// profile and analysis values while those values are refreshed between runs.
class CompilationSession
{
public:
	CompilationSession(): typeMapper(analysis, profile, sourceMap, artifacts) {}

	void begin(
		solidity::frontend::CompilerStack& _compiler,
		std::map<std::string, std::string> const& _sourceAliases,
		TargetProfile _profile)
	{
		awst::NameGen::resetAll();
		functionPointers.reset();
		artifacts.clear();
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

	TargetProfile profile;
	ProgramAnalysis analysis;
	SourceMap sourceMap;
	BuildArtifacts artifacts;
	TypeMapper typeMapper;
	eb::FunctionPointerRegistry functionPointers;
};

} // namespace puyasol::builder
