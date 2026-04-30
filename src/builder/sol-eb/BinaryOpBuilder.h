#pragma once

#include "awst/Node.h"

#include <libsolidity/parsing/Token.h>

#include <memory>

namespace puyasol::builder::eb
{
class BuilderContext;

/// Build an AWST binary-operation expression from already-resolved operands.
///
/// This is the legacy fallback path: SolBinaryOperation calls this after its
/// own per-token routing (user-defined op overload, constant folding, sol-eb
/// type-builder dispatch) fails to handle the operation. It chooses between
/// uint64, biguint, and bytes operations based on operand and result types,
/// and emits any side-effect statements (e.g. the biguint exp loop) into
/// `ctx.prePendingStatements` / `ctx.pendingStatements`.
std::shared_ptr<awst::Expression> buildBinaryOp(
	BuilderContext& _ctx,
	solidity::frontend::Token _op,
	std::shared_ptr<awst::Expression> _left,
	std::shared_ptr<awst::Expression> _right,
	awst::WType const* _resultType,
	awst::SourceLocation const& _loc
);

} // namespace puyasol::builder::eb
