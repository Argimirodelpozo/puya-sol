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
		return layout.totalSlots() != 0 || containsInlineAssembly;
	}

	StorageLayout layout;
	bool containsInlineAssembly = false;
	bool requiresSparseSlots = false;
};

} // namespace puyasol::builder
