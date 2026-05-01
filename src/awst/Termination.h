#pragma once

/// @file Termination.h
/// AWST control-flow analysis helpers used by the builder pipeline:
///   - terminationOf*: does this statement / block always terminate (return / revert)?
///   - removeDeadCode: strip statements after a guaranteed terminator
///
/// `removeDeadCode` is required for puya backend acceptance (puya 5.8.0+
/// rejects unreachable code with a compile error).

#include "awst/Node.h"

#include <memory>
#include <vector>

namespace puyasol::awst
{

/// True if this statement always terminates control flow on every path
/// (a `return` or an `assert(false)` produced by `revert`/`require(false)`).
bool statementAlwaysTerminates(Statement const& _stmt);

/// True if every path through this block reaches a terminator.
/// Recurses through nested blocks and `if/else` where both arms terminate.
bool blockAlwaysTerminates(Block const& _block);

/// Remove unreachable statements that follow a guaranteed terminator inside
/// a block body — recursively into nested blocks, ifs, while/for loops.
void removeDeadCode(std::vector<std::shared_ptr<Statement>>& _body);

} // namespace puyasol::awst
