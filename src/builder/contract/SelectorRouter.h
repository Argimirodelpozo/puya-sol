#pragma once

/// @file SelectorRouter.h
/// Emit the approval-program tail: `return ARC4Router()` (no fallback/receive;
/// can_exit_early=True) or bare-call + router + fallback chain (can_exit_early=False).

#include "awst/Node.h"

#include <libsolidity/ast/ASTForward.h>

namespace puyasol::builder
{

void emitSelectorDispatch(
	awst::Block& _body,
	solidity::frontend::FunctionDefinition const* _fallbackFunc,
	solidity::frontend::FunctionDefinition const* _receiveFunc,
	awst::SourceLocation const& _loc);

} // namespace puyasol::builder
