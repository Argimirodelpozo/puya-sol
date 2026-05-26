#pragma once

/// @file SelectorRouter.h
/// Emits the tail of the approval program — either the simple
/// `return ARC4Router()` pattern (no fallback/receive) or the custom
/// bare-call + router + fallback dispatch chain when the contract
/// defines `fallback()` / `receive()`.
///
/// The simple pattern triggers puya's `can_exit_early=True`
/// optimisation (router rejects on selector miss). The custom
/// dispatch assigns the router result to a var instead, forcing
/// `can_exit_early=False` so the program can fall through to the
/// fallback on miss.

#include "awst/Node.h"

#include <libsolidity/ast/AST.h>

namespace puyasol::builder
{

void emitSelectorDispatch(
	awst::Block& _body,
	solidity::frontend::FunctionDefinition const* _fallbackFunc,
	solidity::frontend::FunctionDefinition const* _receiveFunc,
	awst::SourceLocation const& _loc);

} // namespace puyasol::builder
