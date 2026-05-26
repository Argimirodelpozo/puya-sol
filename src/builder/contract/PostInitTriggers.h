#pragma once

/// @file PostInitTriggers.h
/// Detects whether a contract must defer constructor body execution to
/// a `__postInit` call (run AFTER AppCreate) rather than running it
/// inline during the AppCreate transaction itself.
///
/// Four orthogonal triggers force the deferral:
///   1. Constructor writes to a box-stored state variable (boxes need
///      the app account to exist and hold MBR first).
///   2. Constructor uses `new C(...)` to deploy a child contract
///      (inner-create needs parent balance).
///   3. Constructor references `msg.{value,sender,data}` (these only
///      resolve correctly in the post-init txn group, not AppCreate).
///   4. Constructor calls into `library AVM` from stdlib (inner-txn
///      issuers needing MBR + ASA reserves).

#include <libsolidity/ast/AST.h>

namespace puyasol::builder
{

bool computeNeedsPostInit(solidity::frontend::ContractDefinition const& _contract);

} // namespace puyasol::builder
