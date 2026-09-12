#pragma once

/// @file PostInitTriggers.h
/// True when constructor must defer to __postInit (run after AppCreate).
/// Uses solc creation reachability plus target-specific self/library edges.
/// Box access, child/external calls, message/native context and inline assembly
/// require post-create resources; slot-layout constructors remain conservative.

#include <libsolidity/ast/ASTForward.h>

namespace puyasol::builder
{

class StorageMapper;
struct ProgramAnalysis;

bool computeNeedsPostInit(
	solidity::frontend::ContractDefinition const& _contract,
	StorageMapper const& _storageMapper,
	ProgramAnalysis const& _analysis);

} // namespace puyasol::builder
