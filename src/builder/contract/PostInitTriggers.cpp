#include "builder/contract/PostInitTriggers.h"
#include "builder/ProgramAnalysis.h"
#include "builder/contract/StateVarWalker.h"
#include "builder/storage/StorageMapper.h"
#include "Logger.h"

namespace puyasol::builder
{

bool computeNeedsPostInit(
	solidity::frontend::ContractDefinition const& contract,
	StorageMapper const& storage,
	ProgramAnalysis const& analysis)
{
	// This remains an AVM policy, not a solc reachability fact. In slot mode
	// all mutable initialization and constructor work conservatively defer.
	if (storage.profile().evmStorageLayout)
	{
		for (auto const* base: contract.annotation().linearizedBaseContracts)
		{
			if (base->constructor()) return true;
			for (auto const* variable: base->stateVariables())
				if (variable->value() && !variable->isConstant() && !variable->immutable()) return true;
		}
	}
	auto found = analysis.creationEffects.find(contract.id());
	if (found == analysis.creationEffects.end())
	{
		Logger::instance().error("missing solc creation graph for " + contract.name());
		return true;
	}
	auto const& effects = found->second;
	if (effects.createsContract || effects.externalCall || effects.messageContext
		|| effects.nativeContext || effects.assembly) return true;
	bool boxReference = false;
	forEachStateVar(contract, [&](auto const* variable) {
		if (!variable->isConstant() && effects.stateReferences.count(variable->id())
			&& storage.shouldUseBoxStorage(*variable)) boxReference = true;
	});
	return boxReference;
}

} // namespace puyasol::builder
