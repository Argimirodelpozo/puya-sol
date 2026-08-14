#pragma once

#include "builder/storage/StorageLayout.h"

#include <libsolidity/ast/AST.h>

namespace puyasol::builder
{

/// Storage facts derived once from a contract AST.  Both storage backends use
/// the same runtime-emission predicate, while the EVM backend additionally
/// consumes `requiresSparseSlots` during its unit-wide specialization pass.
struct StorageRuntimePlan
{
	static StorageRuntimePlan analyze(
		solidity::frontend::ContractDefinition const& _contract,
		TypeMapper& _typeMapper);

	bool needsDispatch() const
	{
		// Named AVM state accesses do not use the EVM-word dispatcher. In EVM
		// layout every declared state access does; in default layout only
		// sload/sstore-style assembly access does.
		return evmLayout
			? layout.totalSlots() != 0 || containsInlineAssembly
			: containsInlineAssembly;
	}

	StorageLayout layout;
	bool evmLayout = false;
	bool containsInlineAssembly = false;
	bool requiresSparseSlots = false;
};

} // namespace puyasol::builder
