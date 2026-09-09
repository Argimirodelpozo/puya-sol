#pragma once

#include "awst/Node.h"
#include "builder/sol-types/SolcFwd.h"
#include "builder/storage/StoragePathWalker.h"

namespace puyasol::builder::eb { class ContractContext; }

namespace puyasol::builder::sol_ast
{
struct Context;

/// One path resolver for direct access, aliases and reference arguments.
/// Consumes solc roots/member offsets/array facts and StorageKey's encoder.
/// Unknown shapes return an empty result; callers must not invent a key.
StorageHolder resolveStorageHolder(
	eb::ContractContext& ctx, Context& scope,
	solidity::frontend::Expression const& expression,
	awst::SourceLocation const& loc);

/// Recover a built alias's path without rebuilding its source expression.
/// Pins root keys/indices and returns a fresh value path using those pins;
/// binding that value freezes its location without mutating a shared tree.
StorageHolder resolveBuiltStorageHolder(
	eb::ContractContext& ctx, std::shared_ptr<awst::Expression> const& value,
	awst::SourceLocation const& loc);

/// A key-only reference must preserve both holder identity and the data place.
/// Interior mapping-containing aggregates need a richer handle; reject them
/// instead of passing the enclosing box or a nonexistent descendant data box.
std::shared_ptr<awst::Expression> storageReferenceKey(
	eb::ContractContext& ctx, Context& scope,
	solidity::frontend::Expression const& expression,
	awst::SourceLocation const& loc);

} // namespace puyasol::builder::sol_ast
