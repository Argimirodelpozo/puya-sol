#pragma once
/// @file Clone.h
/// Deep-clone AWST expression/statement trees. Used when a subtree must appear in more than one
/// place as INDEPENDENT nodes — e.g. a modifier with multiple `_;` placeholders runs the function
/// body N times, and the body inliner splices the body once per `_;`. Splicing the same shared_ptr
/// nodes aliases them, so a later in-place pass (checked-arithmetic temps, SingleEvaluation caching)
/// corrupts every copy. cloneExpr/cloneStmt produce structurally-equal but disjoint trees.
///
/// WType pointers are interned/immutable and intentionally SHARED (not cloned). SingleEvaluation ids
/// are re-minted (nextSingleEvalId) so puya's (source,id) eval cache does not merge the copies.
#include "awst/Node.h"

#include <memory>

namespace puyasol::awst
{

std::shared_ptr<Expression> cloneExpr(std::shared_ptr<Expression> const& e);
std::shared_ptr<Statement> cloneStmt(std::shared_ptr<Statement> const& s);
std::shared_ptr<Block> cloneBlock(std::shared_ptr<Block> const& b);

} // namespace puyasol::awst
