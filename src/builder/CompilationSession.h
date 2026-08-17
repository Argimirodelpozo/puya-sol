#pragma once

#include "builder/ProgramAnalysis.h"
#include "builder/SourceLocConvert.h"
#include "builder/BuildArtifacts.h"
#include "builder/TargetProfile.h"
#include "builder/storage/StorageRuntimePlan.h"
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
		TargetProfile _profile);

	/// Return the canonical storage facts for `_contract`, computing them at
	/// most once during this compiler invocation. The unit-wide EVM storage
	/// pre-scan, expression lowering, and runtime-dispatch generation must all
	/// consume this same plan; recomputing it used to walk solc's inheritance
	/// layout and every inline-assembly body three times per concrete contract.
	StorageRuntimePlan const& storagePlan(
		solidity::frontend::ContractDefinition const& _contract)
	{
		auto const found = storagePlans.find(_contract.id());
		if (found != storagePlans.end())
			return found->second;
		auto inserted = storagePlans.emplace(
			_contract.id(), StorageRuntimePlan::analyze(_contract, typeMapper));
		return inserted.first->second;
	}

	TargetProfile profile;
	ProgramAnalysis analysis;
	SourceMap sourceMap;
	BuildArtifacts artifacts;
	std::map<int64_t, StorageRuntimePlan> storagePlans;
	TypeMapper typeMapper;
	eb::FunctionPointerRegistry functionPointers;
};

} // namespace puyasol::builder
