#pragma once

/// @file AwstWalker.h
/// Generic AWST tree walker / rewriter for splitter passes.
///
/// Each pass supplies a `RewriteFn` callback. The walker visits every
/// Expression slot in pre-order; returning nullptr recurses into children,
/// returning a node substitutes it (the callback owns recursion into its
/// replacement).

#include "awst/Node.h"

#include <functional>
#include <memory>

namespace puyasol::splitter
{

/// Per-node callback: return a replacement to substitute, or nullptr to recurse.
using ExprRewriteFn = std::function<
	std::shared_ptr<awst::Expression>(awst::Expression const&)>;

/// Walk all Expression slots in `_block` in pre-order, rewriting via `_fn`.
void walkBlock(awst::Block& _block, ExprRewriteFn const& _fn);

/// Walk all Expression slots in `_stmt` (including child blocks). Mutates in place.
void walkStatement(awst::Statement& _stmt, ExprRewriteFn const& _fn);

/// Walk `_expr` and its subtree in pre-order via `_fn`.
/// `_expr` is by reference because the slot's pointee may be replaced.
void walkExpression(
	std::shared_ptr<awst::Expression>& _expr, ExprRewriteFn const& _fn);

} // namespace puyasol::splitter
