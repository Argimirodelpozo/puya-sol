#pragma once

/// @file SelectorRouter.h
/// Emit the approval-program tail: `return ARC4Router()` (no fallback/receive;
/// can_exit_early=True) or bare-call + router + fallback chain (can_exit_early=False).

#include "awst/Node.h"

#include <libsolidity/ast/ASTForward.h>

namespace puyasol::builder
{

enum class CalldataTransport { SplitEvm, Arc4Arguments, Arc4Fallback };

/// The raw carrier is [selector, body], including empty/short selectors.
/// Arc4Arguments preserves native msg.data concatenation, not EVM ABI.
/// Arc4Fallback accepts only the two-argument raw-call carrier. A selector
/// override permits ARC4-to-Solidity identity translation without another walk.
std::shared_ptr<awst::Expression> reconstructCalldata(
	CalldataTransport transport, awst::SourceLocation const& loc,
	std::shared_ptr<awst::Expression> selector = nullptr);
void emitReturnLog(std::shared_ptr<awst::Expression> payload,
	awst::SourceLocation const& loc, std::vector<std::shared_ptr<awst::Statement>>& out);
void emitFallbackCall(solidity::frontend::FunctionDefinition const& function,
	std::string const& method, std::shared_ptr<awst::Expression> calldata,
	awst::SourceLocation const& loc, std::vector<std::shared_ptr<awst::Statement>>& out);

void emitSelectorDispatch(
	awst::Block& _body,
	solidity::frontend::FunctionDefinition const* _fallbackFunc,
	solidity::frontend::FunctionDefinition const* _receiveFunc,
	awst::SourceLocation const& _loc);

} // namespace puyasol::builder
