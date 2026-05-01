#pragma once

/// @file SubroutineReachability.h
/// Subroutine dead-code elimination at the AWST root-node level.
///
/// AWSTBuilder emits one Subroutine root for every translated free or library
/// function — but most contracts only call a small subset. This pass walks
/// every contract method body, follows `SubroutineCallExpression` targets
/// transitively, and drops any Subroutine root that's never reached.
/// Contracts always survive; non-Subroutine roots survive unchanged.

#include "awst/Node.h"

#include <memory>
#include <vector>

namespace puyasol::builder
{

/// Return only the root nodes reachable from contract methods. Contracts and
/// non-Subroutine roots are kept as-is; unreached Subroutines are dropped.
std::vector<std::shared_ptr<awst::RootNode>> filterToReachableSubroutines(
	std::vector<std::shared_ptr<awst::RootNode>> _roots);

} // namespace puyasol::builder
