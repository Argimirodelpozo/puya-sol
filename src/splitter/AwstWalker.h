#pragma once

/// @file AwstWalker.h
/// Generic AWST tree walker / rewriter — used by the splitter's chunk-
/// patching passes (msg.sender, msg.value, address(this) rewrites).
///
/// Each pass supplies a `RewriteFn` callback. The walker visits every
/// Expression slot reachable from the supplied root in pre-order: the
/// callback is given a chance to substitute, and if it returns nullptr
/// the walker recurses into the node's children. Substituted subtrees
/// are NOT recursed into — the callback is responsible for any inner
/// patching of its own replacement.

#include "awst/Node.h"

#include <functional>
#include <memory>

namespace puyasol::splitter
{

/// Per-node callback. Return a non-null replacement to substitute the
/// node, or nullptr to continue walking into its children.
using ExprRewriteFn = std::function<
	std::shared_ptr<awst::Expression>(awst::Expression const&)>;

/// Walk every Expression slot reachable from `_block` and let `_fn`
/// rewrite each in pre-order. Mutates `_block` in place.
void walkBlock(awst::Block& _block, ExprRewriteFn const& _fn);

/// Walk every Expression slot reachable from `_stmt` (including child
/// blocks for compound statements). Mutates `_stmt` in place.
void walkStatement(awst::Statement& _stmt, ExprRewriteFn const& _fn);

/// Walk every Expression slot reachable from `_expr`. The slot itself
/// is offered to `_fn` first; if `_fn` substitutes, the new subtree is
/// NOT recursed into. Otherwise the walker descends into all child
/// Expression slots in pre-order.
///
/// Pass an `_expr` shared_ptr by reference because the slot's pointee
/// may be replaced wholesale (the callback returns a new shared_ptr).
void walkExpression(
	std::shared_ptr<awst::Expression>& _expr, ExprRewriteFn const& _fn);

} // namespace puyasol::splitter
