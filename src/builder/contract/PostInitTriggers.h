#pragma once

/// @file PostInitTriggers.h
/// True when constructor must defer to __postInit (run after AppCreate).
/// Triggers: (1) box writes (need MBR), (2) new C() deploy,
/// (3) msg.{value,sender,data}, (4) AVM stdlib (inner-txn MBR+ASA).

#include <libsolidity/ast/AST.h>

namespace puyasol::builder
{

bool computeNeedsPostInit(solidity::frontend::ContractDefinition const& _contract);

} // namespace puyasol::builder
