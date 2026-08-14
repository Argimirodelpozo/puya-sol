#pragma once

/// @file SubroutineRegistry.h
/// Whole-unit integrity checks for root-level AWST subroutines. Producers emit
/// helpers on demand; this validates identity and reference invariants without
/// performing post-build dead-code elimination.

#include "awst/Node.h"

#include <memory>
#include <vector>

namespace puyasol::builder
{

/// Log an error for every duplicate subroutine ID or unresolved
/// SubroutineCallExpression target. Returns true when the unit is valid.
bool validateRootSubroutines(
	std::vector<std::shared_ptr<awst::RootNode>> const& _roots);

} // namespace puyasol::builder
